#include "components/contextmenu.h"
#include "core/dsl.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using MenuItem = components::ContextMenuItem;

core::dsl::Element* findSuffix(core::dsl::Element& element, const std::string& suffix) {
    if (element.id.size() >= suffix.size() &&
        element.id.compare(element.id.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return &element;
    }
    for (const std::unique_ptr<core::dsl::Element>& child : element.children) {
        if (core::dsl::Element* found = findSuffix(*child, suffix)) return found;
    }
    return nullptr;
}

core::dsl::Element* findSuffix(core::dsl::Ui& ui, const std::string& suffix) {
    for (const std::unique_ptr<core::dsl::Element>& root : ui.roots()) {
        if (core::dsl::Element* found = findSuffix(*root, suffix)) return found;
    }
    return nullptr;
}

bool sameColor(const core::Color& left, const core::Color& right) {
    constexpr float epsilon = 0.001f;
    return std::fabs(left.r - right.r) < epsilon &&
           std::fabs(left.g - right.g) < epsilon &&
           std::fabs(left.b - right.b) < epsilon &&
           std::fabs(left.a - right.a) < epsilon;
}

struct MenuHarness {
    bool open = true;
    int dispatchCount = 0;
    int dismissCount = 0;
    std::vector<int> selectedPath;
};

void composeMenu(core::dsl::Ui& ui, MenuHarness& state,
                 const std::vector<MenuItem>& items,
                 const components::theme::ThemeColorTokens& tokens,
                 const core::Transition& transition = core::Transition::none(),
                 bool skipClosedContent = false) {
    ui.begin("context-menu-test");
    components::contextMenu(ui, "sample")
        .open(state.open)
        .screen(640.0f, 480.0f)
        .position(30.0f, 28.0f)
        .size(240.0f, 38.0f)
        .items(items)
        .theme(tokens)
        .transition(transition)
        .skipClosedContent(skipClosedContent)
        .onSelectPath([&state](const std::vector<int>& path) {
            ++state.dispatchCount;
            state.selectedPath = path;
        })
        .onOpenChange([&state](bool open) {
            state.open = open;
            if (!open) ++state.dismissCount;
        })
        .build();
    ui.end();
    ui.layout(core::dsl::Screen{640.0f, 480.0f});
}

bool disabledActionDoesNotDispatchOrDismiss() {
    core::dsl::Ui ui;
    MenuHarness state;
    std::vector<MenuItem> items;
    items.emplace_back("不可用");
    items.back().withEnabled(false);
    items.emplace_back("父项", std::vector<MenuItem>{MenuItem("子项")});
    items.back().withSeparatorBefore();
    const components::theme::ThemeColorTokens tokens = components::theme::light();

    composeMenu(ui, state, items, tokens);
    core::dsl::Element* disabled = findSuffix(ui, ".level.0.item.0");
    core::dsl::Element* disabledLabel = findSuffix(ui, ".level.0.label.0");
    if (disabled == nullptr || !disabled->onClick || disabledLabel == nullptr ||
        !sameColor(disabledLabel->textColor, components::ContextMenuStyle(tokens).mutedText)) {
        std::cerr << "disabled menu action did not render as unavailable\n";
        return false;
    }
    disabled->onClick(); // Invoke the same action closure installed on the real DSL element.
    if (state.dispatchCount != 0 || state.dismissCount != 0 || !state.open) {
        std::cerr << "disabled menu action dispatched or dismissed the menu\n";
        return false;
    }
    return true;
}

bool submenuFeedbackAndCloseReopen() {
    core::dsl::Ui ui;
    MenuHarness state;
    std::vector<MenuItem> items;
    items.emplace_back("不可用");
    items.back().withEnabled(false);
    items.emplace_back("父项", std::vector<MenuItem>{MenuItem("子项")});
    items.back().withSeparatorBefore();
    const components::theme::ThemeColorTokens tokens = components::theme::light();
    const components::ContextMenuStyle style(tokens);

    composeMenu(ui, state, items, tokens);
    core::dsl::Element* parent = findSuffix(ui, ".level.0.item.1");
    core::dsl::Element* highlight = findSuffix(ui, ".level.0.highlight");
    core::dsl::Element* level = findSuffix(ui, ".level.0");
    if (parent == nullptr || !parent->onHoverChanged || !parent->onPress || !parent->onRelease ||
        highlight != nullptr || level == nullptr || level->transition.enabled) {
        std::cerr << "submenu row did not expose hover and pressed callbacks\n";
        return false;
    }

    parent->onHoverChanged(true);
    composeMenu(ui, state, items, tokens);
    parent = findSuffix(ui, ".level.0.item.1");
    highlight = findSuffix(ui, ".level.0.highlight");
    if (parent == nullptr || !sameColor(parent->color, core::Color{0.0f, 0.0f, 0.0f, 0.0f}) ||
        highlight == nullptr || !sameColor(highlight->color, style.hover) ||
        highlight->transition.enabled ||
        std::fabs(highlight->frame.y - parent->frame.y) > 0.01f) {
        std::cerr << "hovering a submenu parent did not show a static highlight\n";
        return false;
    }
    parent->onPress(core::PointerEvent{}, core::Rect{});
    composeMenu(ui, state, items, tokens);
    parent = findSuffix(ui, ".level.0.item.1");
    highlight = findSuffix(ui, ".level.0.highlight");
    if (highlight == nullptr || !sameColor(highlight->color, style.pressed)) {
        std::cerr << "submenu parent lost its pressed feedback\n";
        return false;
    }
    parent->onRelease(core::PointerEvent{}, core::Rect{});
    parent->onHoverChanged(false);
    core::dsl::Element* child = findSuffix(ui, ".level.1.item.0");
    if (child == nullptr || !child->onHoverChanged) {
        std::cerr << "submenu child was not composed after opening its parent\n";
        return false;
    }
    child->onHoverChanged(true);
    composeMenu(ui, state, items, tokens);
    highlight = findSuffix(ui, ".level.0.highlight");
    child = findSuffix(ui, ".level.1.item.0");
    core::dsl::Element* childHighlight = findSuffix(ui, ".level.1.highlight");
    if (highlight == nullptr || !sameColor(highlight->color, style.hover) ||
        child == nullptr || childHighlight == nullptr || !sameColor(childHighlight->color, style.hover)) {
        std::cerr << "submenu parent lost its highlight while the child was hovered\n";
        return false;
    }

    child->onHoverChanged(false);
    composeMenu(ui, state, items, tokens);
    child = findSuffix(ui, ".level.1.item.0");
    childHighlight = findSuffix(ui, ".level.1.highlight");
    if (child == nullptr || childHighlight != nullptr || child->transition.enabled ||
        child->color.a > 0.001f) {
        std::cerr << "leaving the last submenu row retained hover fill or animation\n";
        return false;
    }
    child->onHoverChanged(true);
    composeMenu(ui, state, items, tokens);
    child = findSuffix(ui, ".level.1.item.0");

    child->onClick();
    if (state.dispatchCount != 1 || state.selectedPath != std::vector<int>{1, 0} ||
        state.dismissCount != 1 || state.open) {
        std::cerr << "separator changed a command path or the child action did not dismiss\n";
        return false;
    }

    composeMenu(ui, state, items, tokens);
    core::dsl::Element* closedDismissLayer = findSuffix(ui, ".dismiss");
    core::dsl::Element* closedItem = findSuffix(ui, ".level.0.item.1");
    highlight = findSuffix(ui, ".level.0.highlight");
    if (closedDismissLayer != nullptr || closedItem == nullptr || !closedItem->disabled || highlight != nullptr) {
        std::cerr << "closed menu still intercepts pointer input\n";
        return false;
    }
    state.open = true;
    composeMenu(ui, state, items, tokens);
    highlight = findSuffix(ui, ".level.0.highlight");
    level = findSuffix(ui, ".level.0");
    if (highlight != nullptr || level == nullptr || level->transition.enabled) {
        std::cerr << "reopening retained a stale submenu highlight\n";
        return false;
    }
    return true;
}

bool explicitMenuTransitionRemainsAvailable() {
    core::dsl::Ui ui;
    MenuHarness state;
    const std::vector<MenuItem> items{MenuItem("动作")};
    const auto tokens = components::theme::light();
    composeMenu(ui, state, items, tokens, core::Transition::make(0.12f, core::Ease::OutCubic));
    const core::dsl::Element* level = findSuffix(ui, ".level.0");
    if (level == nullptr || !level->transition.enabled ||
        std::fabs(level->transition.durationSeconds - 0.12f) > 0.001f ||
        !core::hasAnimProperty(level->transition.properties, core::AnimProperty::Opacity) ||
        !core::hasAnimProperty(level->transition.properties, core::AnimProperty::Transform)) {
        std::cerr << "explicit menu transition was not applied to the popup level\n";
        return false;
    }
    return true;
}

bool closedContentOptInPreservesReopenAndAnimation() {
    core::dsl::Ui ui;
    MenuHarness state;
    std::vector<MenuItem> items{MenuItem("父项", {MenuItem("子项")})};
    for (int i = 0; i < 20; ++i) items.emplace_back("Action " + std::to_string(i));
    const auto tokens = components::theme::light();
    const auto instant = core::Transition::none();
    const auto animated = core::Transition::make(0.12f, core::Ease::OutCubic);
    composeMenu(ui, state, items, tokens, instant, true);
    auto* parent = findSuffix(ui, ".level.0.item.0");
    if (!parent || !parent->onHoverChanged) return false;
    parent->onHoverChanged(true);
    composeMenu(ui, state, items, tokens, instant, true);
    auto* child = findSuffix(ui, ".level.1.item.0");
    if (!child || !child->onClick) return false;
    child->onClick();
    if (state.open || state.selectedPath != std::vector<int>{0, 0} || state.dispatchCount != 1) return false;
    ui.state<float>("sample.level.0.offset") = 100.0f;
    composeMenu(ui, state, items, tokens, instant, true);
    if (!ui.roots().empty() || ui.state<float>("sample.level.0.offset") != 0.0f) return false;
    state.open = true;
    composeMenu(ui, state, items, tokens, animated, true);
    auto* level = findSuffix(ui, ".level.0");
    if (!level || !level->transition.enabled || findSuffix(ui, ".level.0.highlight") ||
        findSuffix(ui, ".level.1.item.0")) return false;
    auto* viewport = findSuffix(ui, ".level.0.viewport");
    if (!viewport || viewport->scrollOffset != 0.0f) return false;
    state.open = false;
    composeMenu(ui, state, items, tokens, animated, true);
    // Opt in must not omit the tree required for an explicit closing animation.
    auto* closedItem = findSuffix(ui, ".level.0.item.0");
    if (!closedItem || !closedItem->disabled) return false;
    composeMenu(ui, state, items, tokens, instant, true);
    if (!ui.roots().empty()) return false;
    state.open = true;
    composeMenu(ui, state, items, tokens, instant, true);
    parent = findSuffix(ui, ".level.0.item.0");
    if (!parent || !parent->onHoverChanged) return false;
    parent->onHoverChanged(true);
    composeMenu(ui, state, items, tokens, instant, true);
    child = findSuffix(ui, ".level.1.item.0");
    if (!child || !child->onClick) return false;
    child->onClick();
    return state.dispatchCount == 2 && state.selectedPath == std::vector<int>{0, 0};
}

bool shortcutGeometryFitsAtLargeFontAndNarrowWidths() {
    const auto verify = [](float width, float screenWidth, bool expectShortcut) {
        core::dsl::Ui ui;
        auto tokens = components::theme::light();
        tokens.metrics.typography.option = 22.5f; // 150% UI typography.
        tokens.metrics.typography.micro = 16.5f;
        ui.begin("context-menu-geometry");
        components::contextMenu(ui, "geometry")
            .open(true)
            .screen(screenWidth, 240.0f)
            .position(18.0f, 20.0f)
            .size(width, 44.0f)
            .items({MenuItem("复制").withChecked().withShortcut("Ctrl+Shift+P")})
            .theme(tokens)
            .build();
        ui.end();
        ui.layout(core::dsl::Screen{screenWidth, 240.0f});

        const core::dsl::Element* background = findSuffix(ui, ".level.0.bg");
        const core::dsl::Element* check = findSuffix(ui, ".level.0.check.0");
        const core::dsl::Element* label = findSuffix(ui, ".level.0.label.0");
        const core::dsl::Element* shortcut = findSuffix(ui, ".level.0.shortcut.0");
        if (background == nullptr || check == nullptr || label == nullptr ||
            (expectShortcut && shortcut == nullptr) || (!expectShortcut && shortcut != nullptr)) return false;
        const float menuRight = background->frame.x + background->frame.width;
        const bool contentFits = check->frame.x + check->frame.width <= label->frame.x + 0.01f &&
                                 label->frame.height >= 25.5f - 0.01f;
        if (!contentFits) return false;
        if (shortcut == nullptr) return label->frame.x + label->frame.width <= menuRight + 0.01f;
        return shortcut->frame.width >= 80.0f - 0.01f &&
               shortcut->frame.width >= core::TextPrimitive::measureTextWidth(
                   shortcut->text, {}, tokens.metrics.typography.micro) - 0.01f &&
               label->frame.x + label->frame.width <= shortcut->frame.x + 0.01f &&
               shortcut->frame.x >= background->frame.x - 0.01f &&
               shortcut->frame.x + shortcut->frame.width <= menuRight + 0.01f;
    };

    if (!verify(260.0f, 320.0f, true)) {
        std::cerr << "150% shortcut, checkmark, and label geometry overlap or exceed the menu\n";
        return false;
    }
    if (!verify(160.0f, 220.0f, false)) {
        std::cerr << "narrow menu shortcut geometry exceeds the menu bounds\n";
        return false;
    }
    return true;
}

bool checkedAndSubmenuSymbolsUseNativeGeometry() {
    core::dsl::Ui ui;
    MenuHarness state;
    std::vector<MenuItem> items;
    items.emplace_back("父项", std::vector<MenuItem>{MenuItem("子项")});
    items.back().withChecked();
    composeMenu(ui, state, items, components::theme::light());

    const core::dsl::Element* check = findSuffix(ui, ".level.0.check.0");
    const core::dsl::Element* arrow = findSuffix(ui, ".level.0.arrow.0");
    // 勾 = 两段笔画 + 三个收圆端点；箭头 = 两段折线。都是多边形顶点，没有字体节点。
    if (check == nullptr || arrow == nullptr ||
        check->kind != core::dsl::ElementKind::Stack ||
        arrow->kind != core::dsl::ElementKind::Stack ||
        !check->text.empty() || !arrow->text.empty() ||
        check->children.size() != 5 || arrow->children.size() != 2) {
        std::cerr << "checked/submenu symbols lost their semantic root or native geometry nodes\n";
        return false;
    }
    for (const auto& child : check->children) {
        if (child->kind != core::dsl::ElementKind::Polygon || !child->fontFamily.empty()) {
            std::cerr << "menu checkmark unexpectedly uses a text or icon-font element\n";
            return false;
        }
    }
    for (const auto& child : arrow->children) {
        if (child->kind != core::dsl::ElementKind::Polygon || !child->fontFamily.empty()) {
            std::cerr << "submenu arrow unexpectedly uses a text or icon-font element\n";
            return false;
        }
    }
    return true;
}

bool checkableRowsShareAStableLeadingColumn() {
    const auto labelPositions = [](const std::vector<MenuItem>& items) {
        core::dsl::Ui ui;
        MenuHarness state;
        composeMenu(ui, state, items, components::theme::light());
        std::vector<float> positions;
        positions.reserve(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto* label = findSuffix(ui, ".level.0.label." + std::to_string(i));
            if (label == nullptr) return std::vector<float>{};
            positions.push_back(label->frame.x);
        }
        return positions;
    };

    std::vector<MenuItem> mixed;
    mixed.emplace_back("选中");
    mixed.back().withChecked();
    mixed.emplace_back("未选中但可勾选");
    mixed.back().withChecked(false);
    mixed.emplace_back("普通项目");
    const auto mixedX = labelPositions(mixed);
    if (mixedX.size() != 3 || std::fabs(mixedX[0] - mixedX[1]) > 0.01f ||
        std::fabs(mixedX[1] - mixedX[2]) > 0.01f) {
        std::cerr << "checked and unchecked rows did not share the menu label column\n";
        return false;
    }

    std::vector<MenuItem> allUnchecked;
    allUnchecked.emplace_back("复制");
    allUnchecked.back().withChecked(false);
    allUnchecked.emplace_back("粘贴");
    allUnchecked.back().withChecked(false);
    if (!allUnchecked[0].checkable || allUnchecked[0].checked) {
        std::cerr << "withChecked(false) did not retain checkable state\n";
        return false;
    }
    const auto uncheckedX = labelPositions(allUnchecked);
    std::vector<MenuItem> noCheckable;
    noCheckable.emplace_back("普通项目");
    const auto plainX = labelPositions(noCheckable);
    if (uncheckedX.size() != 2 || plainX.size() != 1 ||
        std::fabs(uncheckedX[0] - uncheckedX[1]) > 0.01f ||
        std::fabs(uncheckedX[0] - mixedX[0]) > 0.01f ||
        uncheckedX[0] <= plainX[0] + 0.01f) {
        std::cerr << "all-unchecked checkable rows did not preserve their leading column\n";
        return false;
    }
    return true;
}

} // namespace

// 显式字号子级保留指定几何；普通二级菜单按内容测宽并使用紧凑勾选列。
bool compactChildrenPresentationGeometry() {
    core::dsl::Ui ui;
    MenuHarness state;
    const components::theme::ThemeColorTokens tokens = components::theme::light();
    std::vector<MenuItem> items;
    items.emplace_back("普通项");
    items.emplace_back("字号", std::vector<MenuItem>{MenuItem("12").withChecked(true),
                                                     MenuItem("14"), MenuItem("16")})
        .withChildrenPresentation(components::ContextMenuItem::ChildrenPresentation{112.0f, true, true});
    items.emplace_back("其它", std::vector<MenuItem>{MenuItem("甲").withChecked(true), MenuItem("乙")});

    composeMenu(ui, state, items, tokens);
    core::dsl::Element* compactParent = findSuffix(ui, ".level.0.item.1");
    if (compactParent == nullptr) {
        std::cerr << "compact submenu: parent item missing\n";
        return false;
    }
    compactParent->onHoverChanged(true);
    composeMenu(ui, state, items, tokens);

    core::dsl::Element* level = findSuffix(ui, "sample.level.1");
    core::dsl::Element* label = findSuffix(ui, ".level.1.label.0");
    core::dsl::Element* label1 = findSuffix(ui, ".level.1.label.1");
    core::dsl::Element* check = findSuffix(ui, ".level.1.check.0");
    if (level == nullptr || label == nullptr || label1 == nullptr || check == nullptr) {
        std::cerr << "compact submenu: level/label/check elements missing\n";
        return false;
    }
    if (std::fabs(level->frame.width - 112.0f) > 1.0f) {
        std::cerr << "compact submenu: width=" << level->frame.width << " want 112\n";
        return false;
    }
    // label 左缘 = inset(6) + content(12) + 24 勾选列 = 42；右侧只保留 12 DIP。
    if (std::fabs((label->frame.x - level->frame.x) - 42.0f) > 1.0f ||
        std::fabs(label->frame.width - 52.0f) > 1.0f) {
        std::cerr << "compact submenu: label x=" << (label->frame.x - level->frame.x)
                  << " w=" << label->frame.width << " want 42/52\n";
        return false;
    }
    if (std::fabs(check->frame.width - 16.0f) > 0.6f ||
        std::fabs((check->frame.x - level->frame.x) - 12.0f) > 1.0f) {
        std::cerr << "compact submenu: check size=" << check->frame.width
                  << " x=" << (check->frame.x - level->frame.x) << " want 16/12\n";
        return false;
    }
    if (label->horizontalAlign != core::HorizontalAlign::Center) {
        std::cerr << "compact submenu: number labels are not centered\n";
        return false;
    }
    // 未勾选行与已勾选行的数字盒必须一致（否则勾选行文字会跳位）。
    if (std::fabs(label1->frame.x - label->frame.x) > 0.01f ||
        std::fabs(label1->frame.width - label->frame.width) > 0.01f) {
        std::cerr << "compact submenu: unchecked row label box differs from checked row\n";
        return false;
    }

    // 关闭后隐藏级仍用它自己的宽度，不能在收起过渡时跳回根级的 240。
    state.open = false;
    composeMenu(ui, state, items, tokens, core::Transition::make(0.12f, core::Ease::OutCubic));
    level = findSuffix(ui, "sample.level.1");
    if (level == nullptr || std::fabs(level->frame.width - 112.0f) > 0.01f) {
        std::cerr << "compact submenu: hidden transition lost the resolved width\n";
        return false;
    }
    state.open = true;
    composeMenu(ui, state, items, tokens);
    // 未配置的子级按标签、勾选列和箭头槽自适应宽度。
    core::dsl::Element* plainParent = findSuffix(ui, ".level.0.item.2");
    if (plainParent == nullptr) {
        std::cerr << "compact submenu: plain parent missing\n";
        return false;
    }
    plainParent->onHoverChanged(true);
    composeMenu(ui, state, items, tokens);
    core::dsl::Element* plainLevel = findSuffix(ui, "sample.level.1");
    core::dsl::Element* plainLabel = findSuffix(ui, ".level.1.label.0");
    if (plainLevel == nullptr || plainLabel == nullptr) {
        std::cerr << "compact submenu: plain level missing\n";
        return false;
    }
    const auto expectedPlainWidth = components::context_menu_detail::preferredChildWidth(
        items[2].children, items[2].childrenPresentation, 640.0f,
        components::theme::ThemeMetricTokens{});
    if (std::fabs(plainLevel->frame.width - expectedPlainWidth) > 1.0f ||
        plainLevel->frame.width >= 120.0f) {
        std::cerr << "adaptive submenu: width=" << plainLevel->frame.width
                  << " want " << expectedPlainWidth << " and under 120\n";
        return false;
    }
    // 普通子级的勾选列收紧为 checkColumn+micro（勾居中后视觉缝≈8，与右缘留白平衡）。
    if (std::fabs((plainLabel->frame.x - plainLevel->frame.x) - 34.0f) > 1.0f) {
        std::cerr << "compact submenu: plain label x=" << (plainLabel->frame.x - plainLevel->frame.x)
                  << " want 34 (6+26+2)\n";
        return false;
    }
    return true;
}

int main() {
    {
        core::dsl::Ui ui;
        MenuHarness closed; closed.open = false;
        composeMenu(ui, closed, {MenuItem("Action")}, components::theme::light());
        const auto* level = findSuffix(ui, ".level.0");
        const auto* viewport = findSuffix(ui, ".level.0.viewport");
        if (!level || !viewport || !level->disabled || !viewport->disabled) {
            std::cerr << "closed menu retained an active invisible scroll viewport\n";
            return 1;
        }
    }
    {
        core::dsl::Ui ui;
        ui.begin("menu-bar-dismiss-test");
        components::contextMenu(ui,"bar").open().screen(640,480).position(30,36)
            .dismissTopInset(36).items({"Action"}).build();
        ui.end();
        ui.layout(core::dsl::Screen{640,480});
        const auto* dismiss=findSuffix(ui,".dismiss");
        if(!dismiss || std::fabs(dismiss->frame.y-36)>0.01f || std::fabs(dismiss->frame.height-444)>0.01f) { std::cerr<<"FAIL block1 dismiss="<<!!dismiss<<" y="<<(dismiss?dismiss->frame.y:-1)<<" h="<<(dismiss?dismiss->frame.height:-1)<<"\n"; return 1; }
    }
    {
        core::dsl::Ui ui;
        std::vector<MenuItem> items;
        for(int i=0;i<20;++i) items.emplace_back("Action " + std::to_string(i));
        const auto compose=[&] {
            ui.begin("short-menu-test");
            components::contextMenu(ui,"bar").open().screen(640,220).position(30,40)
                .dismissTopInset(40).items(items).build();
            ui.end();ui.layout(core::dsl::Screen{640,220});
        };
        compose();
        const auto* background=findSuffix(ui,".level.0.bg");
        const auto* first=findSuffix(ui,".level.0.item.0");
        if(!background || !first || background->frame.y<39.9f || background->frame.y+background->frame.height>212.1f) { std::cerr<<"FAIL block2 bg="<<!!background<<" first="<<!!first<<" y="<<(background?background->frame.y:-1)<<" yh="<<(background?background->frame.y+background->frame.height:-1)<<"\n"; return 1; }
        const float originalY=first->frame.y;
        ui.state<float>("bar.level.0.offset")=100;
        compose();first=findSuffix(ui,".level.0.item.0");
        if(!first || std::fabs(first->frame.y-originalY)>.01f) { std::cerr<<"FAIL block3 first="<<!!first<<" y="<<(first?first->frame.y:-1)<<" want="<<originalY<<"\n"; return 1; }
        // 菜单行数超出可见区时，滚动偏移进入渲染期 transform（viewport 持 scrollState，
        // content 绑定同一 id），布局帧保持不变——命中测试由运行时按实例矩阵换算。
        const auto* viewport=findSuffix(ui,".level.0.viewport");
        const auto* content=findSuffix(ui,".level.0.content");
        if(!viewport || !content || viewport->scrollStateId!=viewport->id ||
           std::fabs(viewport->scrollOffset-100.f)>.01f || viewport->scrollMaxOffset<=0.f ||
           content->scrollContentSourceId!=viewport->scrollStateId) {
            std::cerr<<"FAIL block3 wiring viewport="<<!!viewport<<" content="<<!!content
                     <<" stateId="<<viewport->scrollStateId<<" offset="<<(viewport?viewport->scrollOffset:-1.f)
                     <<" max="<<(viewport?viewport->scrollMaxOffset:-1.f)<<"\n";
            return 1;
        }
    }
    if (!disabledActionDoesNotDispatchOrDismiss()) return 1;
    if (!submenuFeedbackAndCloseReopen()) return 1;
    if (!explicitMenuTransitionRemainsAvailable()) return 1;
    if (!closedContentOptInPreservesReopenAndAnimation()) return 1;
    if (!shortcutGeometryFitsAtLargeFontAndNarrowWidths()) return 1;
    if (!checkedAndSubmenuSymbolsUseNativeGeometry()) return 1;
    if (!checkableRowsShareAStableLeadingColumn()) return 1;
    if (!compactChildrenPresentationGeometry()) return 1;
    return 0;
}
