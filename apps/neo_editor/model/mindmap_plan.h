#pragma once

#include "lp_plan.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace neo::mindmap {

enum class Shape { Rectangle, Circle };
enum class RejectReason {
    None, PlanNotReady, NotMarkdown, NoBlock, UnclosedFence, InvalidFence,
    UnsupportedLanguage, NotMindmap, UnsupportedSyntax, InvalidIndent,
    MultipleRoots, EmptyMap, InvalidUtf8, DuplicateId,
    SourceLimit, LabelLimit, DepthLimit, NodeLimit
};

const char* rejectReasonName(RejectReason reason);

struct Limits {
    std::size_t sourceBytes = 256 * 1024;
    std::size_t labelCodepoints = 200;
    std::size_t depth = 32;
    std::size_t nodes = 2000;
};

struct Node {
    std::string label;
    std::string id;
    Shape shape = Shape::Rectangle;
    int parent = -1;
    std::size_t depth = 0;
    std::size_t sourceBeg = 0;
    std::size_t sourceEnd = 0;
    std::vector<int> children;
};

struct Plan {
    std::vector<Node> nodes;
    std::size_t sourceBeg = 0;
    std::size_t sourceEnd = 0;
};

struct ParseResult {
    Plan plan;
    RejectReason rejectReason = RejectReason::None;
    std::size_t sourceLine = 0;
    explicit operator bool() const { return rejectReason == RejectReason::None; }
};

struct Target {
    int blockBeg = -1;
    int blockEnd = -1;
    RejectReason rejectReason = RejectReason::None;
    explicit operator bool() const { return rejectReason == RejectReason::None; }
};

// The supplied LP plan must describe this exact text; this function never rebuilds Markdown.
Target selectTarget(const std::string& text, const LpPlan& plan, int cursor);
ParseResult parseBlock(std::string_view block, std::size_t sourceBeg = 0, const Limits& limits = {});
std::size_t estimatePlanBytes(const Plan& plan);

} // namespace neo::mindmap
