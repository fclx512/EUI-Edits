// mindmap_layout 无头单测：确定性几何、结构/尺寸拒绝、视口穿越与不截断、容量估算。
// 目标需额外编入 apps/neo_editor/model/mindmap_layout.cpp 并加 apps/neo_editor include。

#include "model/mindmap_layout.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace nm = neo::mindmap;

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  !! %s\n", what.c_str());
    }
}

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool sameRect(const nm::Rect& a, const nm::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// 严格重叠（相接不算），用于验证节点框两两不叠。
bool overlaps(const nm::Rect& a, const nm::Rect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

nm::Node makeNode(int parent, std::size_t depth, nm::Shape shape = nm::Shape::Rectangle) {
    nm::Node node;
    node.parent = parent;
    node.depth = depth;
    node.shape = shape;
    return node;
}

// 根 + 两个子（用于几何/连线/视口用例）。
nm::Plan makeSmallPlan() {
    nm::Plan plan;
    plan.nodes.resize(3);
    plan.nodes[0] = makeNode(-1, 0);
    plan.nodes[1] = makeNode(0, 1);
    plan.nodes[2] = makeNode(0, 1);
    plan.nodes[0].children = {1, 2};
    return plan;
}

void testSingleNode() {
    nm::Plan plan;
    plan.nodes.push_back(makeNode(-1, 0));
    nm::Layout layout = nm::buildLayout(plan, {{10.0f, 6.0f}});

    check(layout.valid, "single node: valid");
    check(layout.rejectReason.empty(), "single node: no reject reason");
    check(layout.nodes.size() == 1 && layout.edges.empty(), "single node: one node, no edges");
    // 10 + 2*8 = 26 宽，6 + 2*4 = 14 高，落在原点。
    check(near(layout.nodes[0].bounds.x, 0.0f) && near(layout.nodes[0].bounds.y, 0.0f) &&
              near(layout.nodes[0].bounds.width, 26.0f) && near(layout.nodes[0].bounds.height, 14.0f),
          "single node: padded box is 26x14 at origin");
    check(sameRect(layout.nodes[0].bounds, layout.nodes[0].subtreeBounds),
          "single node: subtree bounds equal node bounds");
    check(near(layout.bounds.width, 26.0f) && near(layout.bounds.height, 14.0f),
          "single node: layout bounds 26x14");
}

void testDeterminismAndGeometry() {
    nm::Plan plan = makeSmallPlan();
    const std::vector<nm::Size> sizes = {{20.0f, 10.0f}, {10.0f, 6.0f}, {12.0f, 8.0f}};
    const nm::Layout a = nm::buildLayout(plan, sizes);
    const nm::Layout b = nm::buildLayout(plan, sizes);

    check(a.valid && b.valid, "determinism: both valid");
    check(a.nodes.size() == b.nodes.size() && a.edges.size() == b.edges.size(),
          "determinism: same counts");
    bool identical = sameRect(a.bounds, b.bounds);
    for (std::size_t i = 0; i < a.nodes.size() && identical; ++i) {
        identical = sameRect(a.nodes[i].bounds, b.nodes[i].bounds) &&
                    sameRect(a.nodes[i].subtreeBounds, b.nodes[i].subtreeBounds);
    }
    for (std::size_t i = 0; i < a.edges.size() && identical; ++i) {
        identical = a.edges[i].parent == b.edges[i].parent && a.edges[i].child == b.edges[i].child &&
                    a.edges[i].pointCount == b.edges[i].pointCount;
        for (int p = 0; p < a.edges[i].pointCount && identical; ++p) {
            identical = a.edges[i].points[p].x == b.edges[i].points[p].x &&
                        a.edges[i].points[p].y == b.edges[i].points[p].y;
        }
    }
    check(identical, "determinism: every field byte-for-byte equal on repeat");

    // 手工核算：root 矩形 36x18，children 26x14 与 28x16。
    // 列 0 宽 36，列 1 起于 36+40=76；children 总高 14+12+16=42；
    // 根竖直居中于 42 → y=(42-18)/2=12；两子树顶 0 与 26。
    check(near(a.nodes[0].bounds.x, 0.0f) && near(a.nodes[0].bounds.y, 12.0f) &&
              near(a.nodes[0].bounds.width, 36.0f) && near(a.nodes[0].bounds.height, 18.0f),
          "geometry: root centered against children column");
    check(near(a.nodes[1].bounds.x, 76.0f) && near(a.nodes[1].bounds.y, 0.0f),
          "geometry: first child at column 1 top");
    check(near(a.nodes[2].bounds.x, 76.0f) && near(a.nodes[2].bounds.y, 26.0f),
          "geometry: second child stacked below first");
    check(near(a.bounds.width, 104.0f) && near(a.bounds.height, 42.0f),
          "geometry: layout bounds cover widest/tallest child");

    // 连线：父右边中点 → 子左边中点，midX 居中，恒 4 点。
    check(a.edges.size() == 2, "geometry: two edges");
    const nm::Edge& e1 = a.edges[0];
    check(e1.parent == 0 && e1.child == 1 && e1.pointCount == 4, "geometry: edge0 identity/4 points");
    check(near(e1.points[0].x, 36.0f) && near(e1.points[0].y, 21.0f) && near(e1.points[3].x, 76.0f) &&
              near(e1.points[3].y, 7.0f),
          "geometry: edge0 anchors parent right-center to child left-center");
    check(near(e1.points[1].x, (36.0f + 76.0f) * 0.5f) && near(e1.points[1].y, 21.0f) &&
              near(e1.points[2].x, (36.0f + 76.0f) * 0.5f) && near(e1.points[2].y, 7.0f),
          "geometry: edge0 orthogonal elbow at midX");

    for (const nm::LayoutNode& node : a.nodes) {
        check(!overlaps(node.bounds, a.nodes[0].bounds) || &node == &a.nodes[0],
              "geometry: nodes do not overlap");
    }
}

void testSiblingSubtreeNoOverlap() {
    // 三层：根 → 两枝，各带两个孙节点，验证兄弟子树包围盒彼此不相交。
    nm::Plan plan;
    plan.nodes.resize(7);
    plan.nodes[0] = makeNode(-1, 0);
    plan.nodes[1] = makeNode(0, 1);
    plan.nodes[2] = makeNode(0, 1);
    plan.nodes[3] = makeNode(1, 2);
    plan.nodes[4] = makeNode(1, 2);
    plan.nodes[5] = makeNode(2, 2);
    plan.nodes[6] = makeNode(2, 2);
    plan.nodes[0].children = {1, 2};
    plan.nodes[1].children = {3, 4};
    plan.nodes[2].children = {5, 6};
    const std::vector<nm::Size> sizes(7, {14.0f, 8.0f});

    const nm::Layout layout = nm::buildLayout(plan, sizes);
    check(layout.valid, "siblings: valid");
    // 兄弟子树（1 与 2）在垂直方向必须分离（相接/重叠都算失败）。
    const nm::Rect& one = layout.nodes[1].subtreeBounds;
    const nm::Rect& two = layout.nodes[2].subtreeBounds;
    check(one.y + one.height <= two.y || two.y + two.height <= one.y,
          "siblings: sibling subtree bounds are disjoint");
    // 全体节点框两两不叠（小树直接两两判）。
    bool clean = true;
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        for (std::size_t j = i + 1; j < layout.nodes.size(); ++j) {
            if (overlaps(layout.nodes[i].bounds, layout.nodes[j].bounds)) {
                clean = false;
            }
        }
    }
    check(clean, "siblings: no two node boxes overlap");
    // 子节点整体位于父节点右侧。
    for (std::size_t i = 1; i < layout.nodes.size(); ++i) {
        const int parent = plan.nodes[i].parent;
        check(layout.nodes[i].bounds.x >=
                  layout.nodes[static_cast<std::size_t>(parent)].bounds.x +
                      layout.nodes[static_cast<std::size_t>(parent)].bounds.width,
              "siblings: child column starts right of parent");
    }
}

void testWideStar2000() {
    constexpr int kTotal = 2000;
    nm::Plan plan;
    plan.nodes.resize(static_cast<std::size_t>(kTotal));
    plan.nodes[0] = makeNode(-1, 0);
    plan.nodes[0].children.reserve(static_cast<std::size_t>(kTotal - 1));
    for (int i = 1; i < kTotal; ++i) {
        plan.nodes[static_cast<std::size_t>(i)] = makeNode(0, 1);
        plan.nodes[0].children.push_back(i);
    }
    const std::vector<nm::Size> sizes(static_cast<std::size_t>(kTotal), {10.0f, 6.0f});

    const nm::Layout layout = nm::buildLayout(plan, sizes);
    check(layout.valid, "wide star: 2000 nodes valid");
    check(layout.nodes.size() == static_cast<std::size_t>(kTotal), "wide star: keeps all nodes");
    check(layout.edges.size() == static_cast<std::size_t>(kTotal - 1), "wide star: N-1 edges");

    bool ordered = true;
    bool disjoint = true;
    for (int i = 1; i + 1 < kTotal; ++i) {
        const nm::Rect& cur = layout.nodes[static_cast<std::size_t>(i)].bounds;
        const nm::Rect& next = layout.nodes[static_cast<std::size_t>(i + 1)].bounds;
        if (!(next.y > cur.y)) ordered = false;
        if (next.y < cur.y + cur.height) disjoint = false;
    }
    check(ordered, "wide star: children stacked in order");
    check(disjoint, "wide star: adjacent siblings disjoint");
    check(near(layout.bounds.height, 1999.0f * 14.0f + 1998.0f * 12.0f),
          "wide star: bounds height equals stacked children span");

    // 超过 2000 上限则整棵拒绝。
    nm::Plan over = plan;
    over.nodes.resize(static_cast<std::size_t>(kTotal + 1));
    over.nodes[static_cast<std::size_t>(kTotal)] = makeNode(0, 1);
    over.nodes[0].children.push_back(kTotal);
    const std::vector<nm::Size> overSizes(static_cast<std::size_t>(kTotal + 1), {10.0f, 6.0f});
    const nm::Layout overLayout = nm::buildLayout(over, overSizes);
    check(!overLayout.valid && overLayout.rejectReason == "node-limit" && overLayout.nodes.empty(),
          "wide star: 2001 nodes rejected with empty output");
}

void testDeepChain() {
    // 33 节点链（depth 0..32）在默认上限内。
    {
        constexpr int kNodes = 33;
        nm::Plan plan;
        plan.nodes.resize(static_cast<std::size_t>(kNodes));
        plan.nodes[0] = makeNode(-1, 0);
        for (int i = 1; i < kNodes; ++i) {
            plan.nodes[static_cast<std::size_t>(i)] = makeNode(i - 1, static_cast<std::size_t>(i));
            plan.nodes[static_cast<std::size_t>(i - 1)].children.push_back(i);
        }
        const std::vector<nm::Size> sizes(static_cast<std::size_t>(kNodes), {10.0f, 6.0f});
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        check(layout.valid, "deep chain: 33 nodes (depth 32) valid");
        check(layout.nodes.size() == static_cast<std::size_t>(kNodes) &&
                  layout.edges.size() == static_cast<std::size_t>(kNodes - 1),
              "deep chain: node/edge counts");
        bool increasing = true;
        for (int i = 1; i < kNodes; ++i) {
            if (layout.nodes[static_cast<std::size_t>(i)].bounds.x <=
                layout.nodes[static_cast<std::size_t>(i - 1)].bounds.x) {
                increasing = false;
            }
        }
        check(increasing, "deep chain: x strictly grows with depth");
    }
    // 34 节点链（depth 到 33）越界。
    {
        constexpr int kNodes = 34;
        nm::Plan plan;
        plan.nodes.resize(static_cast<std::size_t>(kNodes));
        plan.nodes[0] = makeNode(-1, 0);
        for (int i = 1; i < kNodes; ++i) {
            plan.nodes[static_cast<std::size_t>(i)] = makeNode(i - 1, static_cast<std::size_t>(i));
            plan.nodes[static_cast<std::size_t>(i - 1)].children.push_back(i);
        }
        const std::vector<nm::Size> sizes(static_cast<std::size_t>(kNodes), {10.0f, 6.0f});
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        check(!layout.valid && layout.rejectReason == "depth-limit" && layout.nodes.empty(),
              "deep chain: depth 33 rejected as depth-limit");
    }
}

void testCircleCircumscribe() {
    nm::Plan plan;
    plan.nodes.push_back(makeNode(-1, 0, nm::Shape::Circle));
    const nm::Size label{30.0f, 10.0f};
    nm::Options options;
    options.paddingX = 4.0f;
    options.paddingY = 4.0f;
    const nm::Layout layout = nm::buildLayout(plan, {label}, options);

    check(layout.valid, "circle: valid");
    const nm::Rect& box = layout.nodes[0].bounds;
    const float paddedW = label.width + 2.0f * options.paddingX;
    const float paddedH = label.height + 2.0f * options.paddingY;
    const float expectedSide = std::hypot(paddedW, paddedH);
    check(near(box.width, box.height), "circle: bounding box is square");
    check(near(box.width, expectedSide), "circle: side = hypot(label + padding)");
    // 圆的半边长平方应 >= 标签矩形 + padding 的对角线平方的一半（完整包围）。
    const float half = box.width * 0.5f;
    check(half * half >= (paddedW * 0.5f) * (paddedW * 0.5f) + (paddedH * 0.5f) * (paddedH * 0.5f) - 1e-3f,
          "circle: circle radius covers padded label rectangle");
}

void testRejectInvalidStructure() {
    const std::vector<nm::Size> one{{10.0f, 6.0f}};

    // 两个根。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(-1, 0));
        const nm::Layout layout = nm::buildLayout(plan, {one[0], one[0]});
        check(!layout.valid && layout.rejectReason == "root-count", "reject: multiple roots");
    }
    // 父下标越界。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(5, 1));
        const nm::Layout layout = nm::buildLayout(plan, {one[0], one[0]});
        check(!layout.valid && layout.rejectReason == "bad-parent", "reject: parent out of range");
    }
    // 子下标越界。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes[0].children = {5};
        const nm::Layout layout = nm::buildLayout(plan, one);
        check(!layout.valid && layout.rejectReason == "bad-child", "reject: child out of range");
    }
    // 同一子被重复列出。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(0, 1));
        plan.nodes[0].children = {1, 1};
        const nm::Layout layout = nm::buildLayout(plan, {one[0], one[0]});
        check(!layout.valid && layout.rejectReason == "child-dup", "reject: duplicate child");
    }
    // parent 字段与父的 children 不一致。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(0, 1)); // 声称父是 0，但 0 未列出它
        const nm::Layout layout = nm::buildLayout(plan, {one[0], one[0]});
        check(!layout.valid && layout.rejectReason == "parent-mismatch", "reject: parent/children mismatch");
    }
    // depth 不连续。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(0, 5));
        plan.nodes[0].children = {1};
        const nm::Layout layout = nm::buildLayout(plan, {one[0], one[0]});
        check(!layout.valid && layout.rejectReason == "depth-mismatch", "reject: non-contiguous depth");
    }
    // 与根不连通的环。
    {
        nm::Plan plan;
        plan.nodes.resize(4);
        plan.nodes[0] = makeNode(-1, 0);
        plan.nodes[1] = makeNode(0, 1);
        plan.nodes[2] = makeNode(3, 1);
        plan.nodes[3] = makeNode(2, 2);
        plan.nodes[0].children = {1};
        plan.nodes[2].children = {3}; // 2↔3 互为父子，构成与根不连通的环
        plan.nodes[3].children = {2};
        const std::vector<nm::Size> sizes(4, one[0]);
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        check(!layout.valid && layout.rejectReason == "unreachable",
              "reject: cycle disconnected from root is unreachable");
    }
    // 标签尺寸数量不匹配。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        const nm::Layout layout = nm::buildLayout(plan, {});
        check(!layout.valid && layout.rejectReason == "label-size-count", "reject: label count mismatch");
    }
    // 根 depth 必须为 0；否则 depth 连续性与"最深不超过 N-1"不成立，
    // 列宽/坐标数组会被恶意 depth 撑爆。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes[0].depth = std::numeric_limits<std::size_t>::max();
        const nm::Layout layout = nm::buildLayout(plan, one);
        check(!layout.valid && layout.rejectReason == "depth-mismatch" && layout.nodes.empty(),
              "reject: root depth SIZE_MAX is depth-mismatch");
    }
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes[0].depth = 3;
        const nm::Layout layout = nm::buildLayout(plan, one);
        check(!layout.valid && layout.rejectReason == "depth-mismatch", "reject: nonzero root depth");
    }
    // 非法 Shape 枚举。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0, static_cast<nm::Shape>(99)));
        const nm::Layout layout = nm::buildLayout(plan, one);
        check(!layout.valid && layout.rejectReason == "bad-shape" && layout.nodes.empty(),
              "reject: unsupported shape enum");
    }
}

void testGeometryOverflow() {
    const float huge = std::numeric_limits<float>::max();

    // 单靠 label + 默认 padding 不会越过 FLT_MAX：在 double 里 FLT_MAX+16 被舍入回
    // FLT_MAX，不是真溢出。必须让相加真正超过 float 可表示范围。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        nm::Options options;
        options.paddingX = huge;
        const nm::Layout layout = nm::buildLayout(plan, {{huge, 6.0f}}, options);
        check(!layout.valid && layout.rejectReason == "geometry-overflow" && layout.nodes.empty(),
              "overflow: label plus padding exceeds float range");
    }
    // 巨大 padding 单独即可让 label + 2*padding 越界。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        nm::Options options;
        options.paddingX = huge;
        const nm::Layout layout = nm::buildLayout(plan, {{10.0f, 6.0f}}, options);
        check(!layout.valid && layout.rejectReason == "geometry-overflow",
              "overflow: huge finite padding rejected");
    }
    // 列前缀和真实溢出：父列宽 0.6*FLT_MAX + 列间隙 0.6*FLT_MAX = 1.2*FLT_MAX。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        plan.nodes.push_back(makeNode(0, 1));
        plan.nodes[0].children = {1};
        nm::Options options;
        options.columnGap = 0.6f * huge;
        const nm::Layout layout = nm::buildLayout(plan, {{0.6f * huge, 6.0f}, {10.0f, 6.0f}}, options);
        check(!layout.valid && layout.rejectReason == "geometry-overflow",
              "overflow: column prefix sum exceeds float range");
    }
    // 兄弟子树累加真实溢出：rowGap=FLT_MAX、3 个子节点 → 2*FLT_MAX。
    {
        nm::Plan plan;
        plan.nodes.resize(4);
        plan.nodes[0] = makeNode(-1, 0);
        for (int i = 1; i <= 3; ++i) {
            plan.nodes[static_cast<std::size_t>(i)] = makeNode(0, 1);
            plan.nodes[0].children.push_back(i);
        }
        nm::Options options;
        options.rowGap = huge;
        const std::vector<nm::Size> sizes(4, {10.0f, 6.0f});
        const nm::Layout layout = nm::buildLayout(plan, sizes, options);
        check(!layout.valid && layout.rejectReason == "geometry-overflow",
              "overflow: sibling accumulation exceeds float range");
    }
    // 圆节点 hypot 越界（hypot(FLT_MAX, FLT_MAX) ≈ 1.41*FLT_MAX）。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0, nm::Shape::Circle));
        const nm::Layout layout = nm::buildLayout(plan, {{huge, huge}});
        check(!layout.valid && layout.rejectReason == "geometry-overflow",
              "overflow: circle hypot overflow rejected");
    }
}

void testRejectBadInputs() {
    const std::vector<nm::Size> one{{10.0f, 6.0f}};

    // 空树。
    {
        const nm::Layout layout = nm::buildLayout(nm::Plan{}, {});
        check(!layout.valid && layout.rejectReason == "empty-plan" && layout.nodes.empty(),
              "reject: empty plan");
    }
    // NaN / inf / 负尺寸。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        check(!nm::buildLayout(plan, {{nan, 6.0f}}).valid &&
                  nm::buildLayout(plan, {{nan, 6.0f}}).rejectReason == "bad-label-size",
              "reject: NaN label width");
        check(!nm::buildLayout(plan, {{10.0f, inf}}).valid &&
                  nm::buildLayout(plan, {{10.0f, inf}}).rejectReason == "bad-label-size",
              "reject: infinite label height");
        check(!nm::buildLayout(plan, {{-1.0f, 6.0f}}).valid &&
                  nm::buildLayout(plan, {{-1.0f, 6.0f}}).rejectReason == "bad-label-size",
              "reject: negative label width");
    }
    // 非法 options。
    {
        nm::Plan plan;
        plan.nodes.push_back(makeNode(-1, 0));
        nm::Options badPad;
        badPad.paddingX = -1.0f;
        check(!nm::buildLayout(plan, one, badPad).valid &&
                  nm::buildLayout(plan, one, badPad).rejectReason == "bad-options",
              "reject: negative padding");
        nm::Options badGap;
        badGap.columnGap = std::numeric_limits<float>::quiet_NaN();
        check(!nm::buildLayout(plan, one, badGap).valid &&
                  nm::buildLayout(plan, one, badGap).rejectReason == "bad-options",
              "reject: NaN column gap");
        nm::Options badLimit;
        badLimit.maxNodes = 0;
        check(!nm::buildLayout(plan, one, badLimit).valid &&
                  nm::buildLayout(plan, one, badLimit).rejectReason == "bad-options",
              "reject: zero node limit");
    }
}

void testViewportCrossingEdge() {
    nm::Plan plan = makeSmallPlan();
    const std::vector<nm::Size> sizes = {{20.0f, 10.0f}, {10.0f, 6.0f}, {12.0f, 8.0f}};
    const nm::Layout layout = nm::buildLayout(plan, sizes);
    check(layout.valid, "viewport: fixture valid");

    // 视口只卡在父→子第一条水平连线段上（y=21, x∈[36,56]），不碰任何节点框。
    const nm::Rect viewport{40.0f, 18.0f, 8.0f, 6.0f};
    check(!layout.nodes[0].bounds.intersects(viewport) &&
              !layout.nodes[1].bounds.intersects(viewport) &&
              !layout.nodes[2].bounds.intersects(viewport),
          "viewport: no node bounds intersect the gap viewport");
    const nm::Visibility vis = nm::queryVisible(layout, viewport);
    check(vis.nodes.empty(), "viewport: crossing viewport reports no nodes");
    check(vis.edges.size() == 2 && !vis.textOmitted,
          "viewport: both edges whose endpoints are outside but cross are returned");

    // 每个返回的连线两端点都确实在视口外（证明不是"只判端点"）。
    for (int edgeIndex : vis.edges) {
        const nm::Edge& edge = layout.edges[static_cast<std::size_t>(edgeIndex)];
        const nm::Point a = edge.points[0];
        const nm::Point b = edge.points[edge.pointCount - 1];
        const bool aInside = a.x >= viewport.x && a.x <= viewport.x + viewport.width && a.y >= viewport.y &&
                             a.y <= viewport.y + viewport.height;
        const bool bInside = b.x >= viewport.x && b.x <= viewport.x + viewport.width && b.y >= viewport.y &&
                             b.y <= viewport.y + viewport.height;
        check(!aInside && !bInside, "viewport: crossing edge has both endpoints outside");
    }

    // 非有限视口返回空。
    const nm::Visibility bad = nm::queryVisible(
        layout, nm::Rect{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 10.0f});
    check(bad.nodes.empty() && bad.edges.empty(), "viewport: non-finite viewport yields empty result");
}

void testViewportAllVisibleNoTruncation() {
    // 小布局：全在视口内，不触发省略。
    {
        nm::Plan plan = makeSmallPlan();
        const std::vector<nm::Size> sizes = {{20.0f, 10.0f}, {10.0f, 6.0f}, {12.0f, 8.0f}};
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        const nm::Visibility vis = nm::queryVisible(layout, layout.bounds);
        check(vis.nodes.size() == 3 && vis.edges.size() == 2 && !vis.textOmitted,
              "viewport: small layout fully visible without omission");
    }
    // 2000 星：视口为整张图 → 全部节点/连线返回，不截断，仅建议省略文字。
    {
        constexpr int kTotal = 2000;
        nm::Plan plan;
        plan.nodes.resize(static_cast<std::size_t>(kTotal));
        plan.nodes[0] = makeNode(-1, 0);
        for (int i = 1; i < kTotal; ++i) {
            plan.nodes[static_cast<std::size_t>(i)] = makeNode(0, 1);
            plan.nodes[0].children.push_back(i);
        }
        const std::vector<nm::Size> sizes(static_cast<std::size_t>(kTotal), {10.0f, 6.0f});
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        const nm::Visibility vis = nm::queryVisible(layout, layout.bounds);
        check(vis.nodes.size() == static_cast<std::size_t>(kTotal),
              "viewport: 2000-node star keeps all visible nodes (no truncation)");
        check(vis.edges.size() == static_cast<std::size_t>(kTotal - 1),
              "viewport: 2000-node star keeps all edges");
        check(vis.textOmitted, "viewport: >300 visible nodes only suggests text omission");
    }
}

void testQueryVisibleRobustness() {
    // 外部自行构造的 Layout：pointCount 可能超出 points[4]，查询必须截断而不是越界读。
    nm::Layout fake;
    fake.valid = true;
    fake.bounds = nm::Rect{0.0f, 0.0f, 20.0f, 20.0f};
    fake.nodes.push_back(nm::LayoutNode{nm::Rect{0.0f, 0.0f, 5.0f, 5.0f}, nm::Rect{0.0f, 0.0f, 10.0f, 10.0f}});
    nm::Edge oversized;
    oversized.parent = 0;
    oversized.child = 0;
    oversized.pointCount = 6; // 越界；保守只取前 4 点
    oversized.points[0] = nm::Point{0.0f, 0.0f};
    oversized.points[1] = nm::Point{5.0f, 5.0f};
    fake.edges.push_back(oversized);
    nm::Edge empty;
    empty.pointCount = 0; // 无有效段，不参与命中
    fake.edges.push_back(empty);
    nm::Edge negative;
    negative.pointCount = -5; // 负值也必须安全夹紧
    fake.edges.push_back(negative);

    const nm::Visibility vis = nm::queryVisible(fake, nm::Rect{0.0f, 0.0f, 10.0f, 10.0f});
    check(vis.nodes.size() == 1, "robustness: node reported for hand-built layout");
    check(vis.edges.size() == 1 && vis.edges[0] == 0,
          "robustness: oversized pointCount clamped, zero-segment edge skipped");

    // 有限但巨大的视口：右/下边界相加溢出 → 空结果，不产生 NaN 命中。
    {
        const float huge = std::numeric_limits<float>::max();
        nm::Plan plan = makeSmallPlan();
        const std::vector<nm::Size> sizes = {{20.0f, 10.0f}, {10.0f, 6.0f}, {12.0f, 8.0f}};
        const nm::Layout layout = nm::buildLayout(plan, sizes);
        const nm::Visibility overflow = nm::queryVisible(layout, nm::Rect{huge, 0.0f, huge, 10.0f});
        check(overflow.nodes.empty() && overflow.edges.empty(),
              "robustness: overflow-causing finite viewport yields empty result");
    }
}

void testEdgeMidpointNoOverflow() {
    // 两个各自有限的 float 大端点相加仍可能溢出：parentRight≈0.25*FLT_MAX、
    // childLeft≈0.85*FLT_MAX，float 相加得 1.1*FLT_MAX（inf）。必须在 double 里求
    // 中点，写回后仍有限。
    const float huge = std::numeric_limits<float>::max();
    nm::Plan plan;
    plan.nodes.push_back(makeNode(-1, 0));
    plan.nodes.push_back(makeNode(0, 1));
    plan.nodes[0].children = {1};
    const std::vector<nm::Size> sizes = {{0.25f * huge, 6.0f}, {10.0f, 6.0f}};
    nm::Options options;
    options.columnGap = 0.6f * huge; // childLeft ≈ 0.85*FLT_MAX

    const nm::Layout layout = nm::buildLayout(plan, sizes, options);
    check(layout.valid, "edge midpoint: large column gap still valid");
    check(layout.edges.size() == 1 && layout.edges[0].pointCount == 4,
          "edge midpoint: one four-point edge");
    if (layout.valid && layout.edges.size() == 1) {
        const nm::Edge& edge = layout.edges[0];
        bool finite = true;
        for (int p = 0; p < edge.pointCount; ++p) {
            finite = finite && std::isfinite(edge.points[p].x) && std::isfinite(edge.points[p].y);
        }
        check(finite, "edge midpoint: all points finite");
        const double parentRight = static_cast<double>(layout.nodes[0].bounds.x) +
                                   static_cast<double>(layout.nodes[0].bounds.width);
        const double childLeft = static_cast<double>(layout.nodes[1].bounds.x);
        const double expected = (parentRight + childLeft) * 0.5;
        const float tolerance = static_cast<float>(expected * 1e-5);
        check(near(edge.points[1].x, static_cast<float>(expected), tolerance),
              "edge midpoint: elbow equals the double midpoint");
        check(edge.points[1].x == edge.points[2].x, "edge midpoint: both elbows share x");
    }
    // float 表示下两个节点框的右边界仍须有限。
    for (const nm::LayoutNode& node : layout.nodes) {
        const double right = static_cast<double>(node.bounds.x) + static_cast<double>(node.bounds.width);
        check(std::isfinite(right) && right <= static_cast<double>(huge),
              "edge midpoint: node bounds right stays finite");
    }
}

void testEstimateLayoutBytes() {
    nm::Plan plan = makeSmallPlan();
    const std::size_t nodeCount = plan.nodes.size();
    const std::vector<nm::Size> sizes(nodeCount, {10.0f, 6.0f});
    const nm::Layout layout = nm::buildLayout(plan, sizes);

    const std::size_t fromLayout = nm::estimateLayoutBytes(layout);
    const std::size_t payload =
        layout.nodes.capacity() * sizeof(nm::LayoutNode) + layout.edges.capacity() * sizeof(nm::Edge);
    check(fromLayout >= payload, "estimate: layout estimate covers capacity payload");
    check(fromLayout >= nodeCount * sizeof(nm::LayoutNode), "estimate: at least node payload");

    const std::size_t planned = nm::estimateLayoutBytes(nodeCount);
    check(planned >= nodeCount * sizeof(nm::LayoutNode) + (nodeCount - 1) * sizeof(nm::Edge),
          "estimate: planned estimate covers nodes plus tree edges");
    check(nm::estimateLayoutBytes(static_cast<std::size_t>(2000)) >
              nm::estimateLayoutBytes(static_cast<std::size_t>(1000)),
          "estimate: grows with node count");
    check(nm::estimateLayoutBytes(static_cast<std::size_t>(0)) == sizeof(nm::Layout),
          "estimate: zero nodes costs only the layout header");
}

} // namespace

int main() {
    testSingleNode();
    testDeterminismAndGeometry();
    testSiblingSubtreeNoOverlap();
    testWideStar2000();
    testDeepChain();
    testCircleCircumscribe();
    testRejectInvalidStructure();
    testRejectBadInputs();
    testGeometryOverflow();
    testViewportCrossingEdge();
    testViewportAllVisibleNoTruncation();
    testQueryVisibleRobustness();
    testEdgeMidpointNoOverflow();
    testEstimateLayoutBytes();

    std::printf("\n%s：%d 项检查，%d 项失败\n", gFailures == 0 ? "ALL PASS" : "FAILED", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
