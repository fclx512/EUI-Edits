#include "model/lp_plan.h"
#include "model/mindmap_plan.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>

int main() {
    const std::string path = std::string(EUI_TEST_SOURCE_DIR) + "/tests/assets/mindmap/manual-preview.md";
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) { std::cerr << "Cannot read manual sample: " << path << '\n'; return 1; }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto lp = neo::buildLpPlan(text);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    std::size_t at = 0;
    int accepted = 0, rejected = 0, otherDiagram = 0;
    while ((at = text.find("```mermaid\n", at)) != std::string::npos) {
        const auto target = neo::mindmap::selectTarget(text, lp, static_cast<int>(at));
        if (target) {
            const auto result = neo::mindmap::parseBlock(std::string_view(text.data() + target.blockBeg,
                static_cast<std::size_t>(target.blockEnd - target.blockBeg)), target.blockBeg);
            if (result) {
                ++accepted;
                check(!result.plan.nodes.empty(), "Accepted manual sample has no nodes");
                std::cout << "accepted root=" << result.plan.nodes.front().label
                          << " nodes=" << result.plan.nodes.size() << '\n';
            } else if (result.rejectReason == neo::mindmap::RejectReason::NotMindmap) {
                ++otherDiagram;
            } else {
                ++rejected;
                check(result.rejectReason == neo::mindmap::RejectReason::UnsupportedSyntax,
                      "Unsupported manual sample must be rejected for its syntax");
            }
        } else if (target.rejectReason == neo::mindmap::RejectReason::NotMindmap) {
            ++otherDiagram;
        } else {
            ++rejected;
            check(target.rejectReason == neo::mindmap::RejectReason::UnsupportedSyntax,
                  "Unsupported manual sample must be rejected for its syntax");
            std::cout << "rejected reason=" << neo::mindmap::rejectReasonName(target.rejectReason) << '\n';
        }
        at += 11;
    }
    check(accepted == 3, "Expected three accepted mindmap samples");
    check(rejected == 1, "Expected one unsupported-syntax sample");
    check(otherDiagram == 1, "Expected one non-mindmap Mermaid sample");
    return failures ? 1 : 0;
}
