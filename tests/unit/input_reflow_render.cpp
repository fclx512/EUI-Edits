// Real InputBuilder and Runtime text-retention regression for dense Live Preview.
#include "components/input.h"
#include "core/dsl_runtime.h"
#include "model/lp_decorations.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// Runtime deliberately has no public instance inspection API. This friend is
// read-only and lives solely in this test translation unit.
namespace core::dsl {
struct RuntimeTestAccess {
    static const std::unordered_map<std::string, runtime::TextInstance>& texts(const Runtime& host) {
        return host.instances_.texts;
    }
};
} // namespace core::dsl

namespace {
using Model = components::input_detail::InputModel;
using State = Model::InputState;
using Decoration = components::input_detail::LineDecoration;
using Element = core::dsl::Element;

int failures = 0;
void check(bool valid, const std::string& reason) {
    if (!valid) {
        ++failures;
        if (failures <= 20) std::cerr << "FAIL: " << reason << '\n';
    }
}
bool near(float a, float b) { return std::fabs(a - b) <= 0.02f; }
bool sameColor(core::Color a, core::Color b) {
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

std::string denseMarkdown() {
    // Same repeated inline mix as dense-20000-codepoints.md. Keep the fixture
    // self-contained so this unit test does not depend on an out/ artifact.
    constexpr const char* phrase =
        "**中英 Bold**、*斜体 Italic*、`代码_x`、[链接文字](note.md)、~~删除文字~~，中文🙂。";
    std::string text = "# Markdown resize fixture\n\n";
    for (int row = 0; row < 48; ++row) {
        for (int repeat = 0; repeat < 9; ++repeat) text += phrase;
        text += "\n\n";
    }
    return text;
}

struct Node {
    std::string id;
    core::dsl::ElementKind kind = core::dsl::ElementKind::Text;
    std::string text;
    core::LayoutRect frame{};
    float x = 0.0f, y = 0.0f;
    std::string fontFamily;
    float fontSize = 0.0f, lineHeight = 0.0f;
    int fontWeight = 0;
    bool fontItalic = false, wrap = false;
    core::Color color{};
    core::Border border{};
    float radius = 0.0f;
};

void collect(const Element& element, std::vector<Node>& nodes) {
    // Text spans plus their chip, strike and underline rectangles describe the
    // actual InputBuilder draw list. Line geometry alone misses stale run text.
    if (element.id.find(".editor.text.") != std::string::npos &&
        (element.kind == core::dsl::ElementKind::Text ||
         element.kind == core::dsl::ElementKind::Rect)) {
        Node node;
        node.id = element.id;
        node.kind = element.kind;
        node.text = element.text;
        node.frame = element.frame;
        node.x = element.x;
        node.y = element.y;
        node.fontFamily = element.fontFamily;
        node.fontSize = element.fontSize;
        node.fontWeight = element.fontWeight;
        node.fontItalic = element.fontItalic;
        node.lineHeight = element.lineHeight;
        node.wrap = element.wrap;
        node.color = element.kind == core::dsl::ElementKind::Text ? element.textColor : element.color;
        node.border = element.border;
        node.radius = element.radius;
        nodes.push_back(std::move(node));
    }
    for (const auto& child : element.children) collect(*child, nodes);
}

std::vector<Node> snapshot(const core::dsl::Ui& ui) {
    std::vector<Node> nodes;
    for (const auto& root : ui.roots()) collect(*root, nodes);
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.id < b.id; });
    return nodes;
}

void compareNodes(const std::vector<Node>& actual, const std::vector<Node>& oracle,
                  const std::string& stage) {
    check(actual.size() == oracle.size(), stage + ": draw-element count");
    for (std::size_t i = 0; i < std::min(actual.size(), oracle.size()); ++i) {
        const auto& a = actual[i];
        const auto& b = oracle[i];
        const std::string where = stage + ": " + a.id;
        check(a.id == b.id && a.kind == b.kind, where + " identity/kind");
        check(a.text == b.text, where + " visible UTF-8 bytes");
        check(near(a.x, b.x) && near(a.y, b.y) &&
              near(a.frame.x, b.frame.x) && near(a.frame.y, b.frame.y) &&
              near(a.frame.width, b.frame.width) && near(a.frame.height, b.frame.height),
              where + " position/frame");
        check(a.fontFamily == b.fontFamily && near(a.fontSize, b.fontSize) &&
              a.fontWeight == b.fontWeight && a.fontItalic == b.fontItalic &&
              near(a.lineHeight, b.lineHeight) && a.wrap == b.wrap &&
              sameColor(a.color, b.color), where + " text/style");
        check(near(a.border.width, b.border.width) &&
              sameColor(a.border.color, b.border.color) && near(a.radius, b.radius),
              where + " chip/decoration style");
    }
}

struct Editor {
    core::dsl::Runtime runtime;
    core::dsl::Ui* ui = nullptr;
    State* state = nullptr;
    const std::string& text;
    const std::vector<Decoration>& decorations;
    bool resumable = false;

    Editor(const std::string& source, const std::vector<Decoration>& rows, bool async)
        : text(source), decorations(rows), resumable(async) { runtime.initialize(); }
    ~Editor() { runtime.shutdown(); }

    void compose(float width, std::uint64_t serial) {
        runtime.compose("reflow-render", 1200.0f, 720.0f,
            [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                ui = &view;
                auto& current = view.state<State>("editor");
                current.followCaret = false;
                current.wordWrap = true;
                auto input = components::input(view, "editor");
                input.position(12.0f, 8.0f).size(width, 680.0f)
                    .inset(12.0f).fontSize(16.0f).fontFamily("Microsoft YaHei")
                    .multiline(true).wordWrap(true).lineNumbers(true).scrollbar(true)
                    .value(text)
                    .lineDecorator([this](const std::string&) { return decorations; });
                if (resumable) input.widthReflowFrameToken(serial);
                input.build();
            });
        state = &ui->state<State>("editor");
        runtime.update(nullptr, 0.0f, 1.0f, 1.0f);
    }

    void pauseScheduler() {
        ui->state<components::InputWidthReflowSchedule>("editor.width-reflow-schedule")
            .nextSliceDue = std::chrono::steady_clock::time_point::max();
    }
};

void compareRuntimeToElements(const Editor& editor, const std::vector<Node>& nodes,
                              const std::string& stage) {
    const auto& instances = core::dsl::RuntimeTestAccess::texts(editor.runtime);
    std::size_t textCount = 0;
    for (const Node& node : nodes) {
        if (node.kind != core::dsl::ElementKind::Text) continue;
        ++textCount;
        const auto it = instances.find(node.id);
        check(it != instances.end(), stage + ": retained TextInstance exists for " + node.id);
        if (it == instances.end()) continue;
        const auto& actual = it->second;
        check(actual.text == node.text, stage + ": retained text bytes for " + node.id);
        check(actual.fontFamily == node.fontFamily && near(actual.fontSize, node.fontSize) &&
              actual.fontWeight == node.fontWeight && actual.fontItalic == node.fontItalic &&
              near(actual.lineHeight, node.lineHeight) && actual.wrap == node.wrap,
              stage + ": retained font/style for " + node.id);
        const auto frame = actual.frame.value();
        check(near(frame.x, node.frame.x) && near(frame.y, node.frame.y) &&
              near(frame.width, node.frame.width) && near(frame.height, node.frame.height),
              stage + ": retained draw position for " + node.id);
        check(sameColor(actual.color.value(), node.color),
              stage + ": retained draw color for " + node.id);
    }
    check(textCount > 100, stage + ": fixture emits many visible styled text nodes");
}

void drain(Editor& editor) {
    check(Model::widthReflowPending(*editor.state), "resize starts a private width draft");
    for (int slice = 0; slice < 100000 && Model::widthReflowPending(*editor.state); ++slice) {
        Model::stepWidthReflow(*editor.state, std::chrono::steady_clock::time_point::max(), 4096);
    }
    check(!Model::widthReflowPending(*editor.state), "width draft eventually publishes");
}

void testDenseMarkdownRenderRetention() {
    const std::string text = denseMarkdown();
    const neo::LpPlan plan = neo::buildLpPlan(text);
    check(plan.stats.invalidRanges == 0 && plan.stats.usedMd4c,
          "dense Markdown has valid parsed inline spans");
    components::MarkdownStyle style;
    style.fontFamily = "Microsoft YaHei";
    style.codeFontFamily = "Consolas";
    style.bodySize = 16.0f;
    style.bodyLineHeight = 24.0f;
    style.inlineCodeBackground = {0.88f, 0.92f, 0.98f, 1.0f};
    style.inlineCodeBorder = {0.65f, 0.72f, 0.86f, 1.0f};
    std::vector<Decoration> rows;
    neo::lp::buildDecorations(plan, style, static_cast<int>(text.size()), rows,
                              style.fontFamily, {}, nullptr, text);
    check(rows.size() == plan.lines.size(), "dense Markdown yields a full LP decoration table");

    constexpr float wide = 1000.0f, narrow = 830.0f;
    Editor oracleWide(text, rows, false);
    Editor oracleNarrow(text, rows, false);
    Editor actual(text, rows, true);
    oracleWide.compose(wide, 1);
    oracleNarrow.compose(narrow, 1);
    actual.compose(wide, 1);
    const auto wideNodes = snapshot(*oracleWide.ui);
    const auto narrowNodes = snapshot(*oracleNarrow.ui);
    const auto initialNodes = snapshot(*actual.ui);
    const auto hasNode = [&wideNodes](const auto& predicate) {
        return std::any_of(wideNodes.begin(), wideNodes.end(), predicate);
    };
    check(hasNode([](const Node& n) { return n.kind == core::dsl::ElementKind::Text && n.fontWeight >= 600; }),
          "fixture renders bold text spans");
    check(hasNode([](const Node& n) { return n.kind == core::dsl::ElementKind::Text && n.fontItalic; }),
          "fixture renders italic text spans");
    check(hasNode([](const Node& n) { return n.id.find(".bg") != std::string::npos; }),
          "fixture renders inline-code chips");
    check(hasNode([](const Node& n) { return n.id.find(".strike") != std::string::npos; }),
          "fixture renders strikethrough strokes");
    check(hasNode([](const Node& n) { return n.id.find(".underline") != std::string::npos; }),
          "fixture renders link underlines");
    compareNodes(initialNodes, wideNodes, "initial wide compose");
    compareRuntimeToElements(actual, initialNodes, "initial wide compose");
    compareRuntimeToElements(oracleNarrow, narrowNodes, "cold narrow oracle");

    bool changedSharedRun = false;
    for (const Node& wideNode : wideNodes) {
        const auto match = std::lower_bound(narrowNodes.begin(), narrowNodes.end(), wideNode.id,
            [](const Node& node, const std::string& id) { return node.id < id; });
        if (match != narrowNodes.end() && match->id == wideNode.id &&
            match->kind == core::dsl::ElementKind::Text && match->text != wideNode.text) {
            changedSharedRun = true;
            break;
        }
    }
    check(changedSharedRun, "resize reuses at least one text element ID with changed bytes");

    actual.pauseScheduler();
    actual.compose(narrow, 2); // A complete old layout remains visible while B is private.
    drain(actual);
    actual.compose(narrow, 3);
    const auto publishedNarrow = snapshot(*actual.ui);
    compareNodes(publishedNarrow, narrowNodes, "published narrow compose");
    compareRuntimeToElements(actual, publishedNarrow, "published narrow compose");

    actual.pauseScheduler();
    actual.compose(wide, 4);
    drain(actual);
    actual.compose(wide, 5);
    const auto republishedWide = snapshot(*actual.ui);
    compareNodes(republishedWide, wideNodes, "republished wide compose");
    compareRuntimeToElements(actual, republishedWide, "republished wide compose");
}
} // namespace

int main() {
    testDenseMarkdownRenderRetention();
    if (failures) std::cerr << failures << " input reflow render failures\n";
    return failures ? 1 : 0;
}
