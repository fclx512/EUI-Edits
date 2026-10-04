#pragma once

#include "components/theme.h"
#include "components/vector_icon.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace components {

struct ContextMenuStyle {
    ContextMenuStyle() : ContextMenuStyle(theme::dark()) {}

    explicit ContextMenuStyle(const theme::ThemeColorTokens& tokens) {
        background = tokens.dark
            ? core::mixColor(tokens.surface, theme::color(0.0f, 0.0f, 0.0f), 0.16f)
            : tokens.surface;
        hover = tokens.surfaceHover;
        pressed = tokens.surfaceActive;
        text = tokens.text;
        mutedText = theme::withOpacity(tokens.text, 0.54f);
        border = theme::withOpacity(tokens.border, 0.82f);
        shadow = theme::popupShadow(tokens);
        radius = tokens.metrics.radius.card;
    }

    core::Color background;
    core::Color hover;
    core::Color pressed;
    core::Color text;
    core::Color mutedText;
    core::Color border;
    core::Shadow shadow;
    float radius = 12.0f;
};

struct ContextMenuItem {
    // 子级（这一项展开成的那一层菜单）的呈现意图。只有需要特例的父项才设置它；
    // 不设置时 preferredWidth=0 / compactCheckColumn=false / centerLabels=false，
    // 使用按内容测量的宽度、26 勾选列和左对齐标签（子级里勾选列与标签只留 micro 缝）。
    //   · preferredWidth     >0 = 该子级请求的绝对宽度（仍受屏宽安全边界约束）
    //   · compactCheckColumn =  勾选列收窄为 24、勾选图标 16，并在该列内居中
    //   · centerLabels       =  标签在其可用区域内水平居中（主菜单文字仍左对齐）
    struct ChildrenPresentation {
        float preferredWidth = 0.0f;
        bool compactCheckColumn = false;
        bool centerLabels = false;
    };

    std::string text;
    std::vector<ContextMenuItem> children;
    // Optional application command; independent of the visible row/path order.
    int commandId = 0;
    // A separator belongs to the following action, so logical menu paths stay stable.
    bool separatorBefore = false;
    bool enabled = true;
    bool checkable = false;
    bool checked = false;
    std::string shortcut;
    // 展开这一项时子级菜单的呈现配置（见 ChildrenPresentation）。
    ChildrenPresentation childrenPresentation;

    ContextMenuItem(std::string value) : text(std::move(value)) {}
    ContextMenuItem(std::string value, std::vector<ContextMenuItem> childItems)
        : text(std::move(value)), children(std::move(childItems)) {}

    bool hasChildren() const { return !children.empty(); }
    ContextMenuItem& withSeparatorBefore(bool value = true) { separatorBefore = value; return *this; }
    ContextMenuItem& withEnabled(bool value) { enabled = value; return *this; }
    ContextMenuItem& withChecked(bool value = true) { checkable = true; checked = value; return *this; }
    ContextMenuItem& withShortcut(std::string value) { shortcut = std::move(value); return *this; }
    ContextMenuItem& withCommand(int value) { commandId = value; return *this; }
    ContextMenuItem& withChildrenPresentation(ChildrenPresentation value) {
        childrenPresentation = value;
        return *this;
    }
};

namespace context_menu_detail {

struct MenuItemSlots {
    float leading = 0.0f;
    float trailing = 0.0f;
    float shortcut = 0.0f;
    float shortcutGap = 0.0f;
};

inline MenuItemSlots menuItemSlots(const ContextMenuItem& item,
                                   bool hasCheckColumn,
                                   const ContextMenuItem::ChildrenPresentation& presentation,
                                   const theme::ThemeMetricTokens& metrics,
                                   bool showShortcut,
                                   bool childLevel = false) {
    const float checkColumn = presentation.compactCheckColumn ? 24.0f : 26.0f;
    const float shortcutWidth = showShortcut
        ? std::max(80.0f, core::TextPrimitive::measureTextWidth(
              item.shortcut, {}, metrics.typography.micro))
        : 0.0f;
    // 勾选列后的标签缝：根级与紧凑呈现（compactCheckColumn）留 content 缝——
    // 字号子级的 42 标签列是已定稿的几何；其余子级只留 micro 缝——
    // 勾图标在列内居中后，视觉缝≈8 DIP，与面板右缘留白平衡。
    const float leading = hasCheckColumn
        ? (childLevel && !presentation.compactCheckColumn
              ? checkColumn + metrics.spacing.micro
              : metrics.spacing.content + checkColumn)
        : metrics.spacing.content;
    return {
        leading,
        item.hasChildren() ? metrics.control.compact + metrics.spacing.micro
                           : metrics.spacing.content,
        shortcutWidth,
        showShortcut ? metrics.spacing.content : 0.0f
    };
}

inline float menuHeight(std::size_t itemCount, float itemHeight, float inset) {
    return itemHeight * static_cast<float>(itemCount) + inset * 2.0f;
}

inline float menuHeight(const std::vector<ContextMenuItem>& items, float itemHeight,
                        float inset, float separatorGap) {
    float height = inset * 2.0f;
    for (const ContextMenuItem& item : items) {
        height += itemHeight + (item.separatorBefore ? separatorGap : 0.0f);
    }
    return height;
}

inline float menuItemTop(const std::vector<ContextMenuItem>& items, std::size_t index,
                         float itemHeight, float inset, float separatorGap) {
    float top = inset;
    const std::size_t limit = std::min(index, items.size());
    for (std::size_t i = 0; i < limit; ++i) {
        top += itemHeight + (items[i].separatorBefore ? separatorGap : 0.0f);
    }
    if (index < items.size() && items[index].separatorBefore) top += separatorGap;
    return top;
}

inline float clampMenuY(float desiredY, float height, float screenHeight) {
    return std::clamp(desiredY, 8.0f, std::max(8.0f, screenHeight - height - 8.0f));
}

inline float childMenuX(float parentX, float parentWidth, float childWidth, float screenWidth, float gap) {
    const float right = parentX + parentWidth + gap;
    if (right + childWidth + 8.0f <= screenWidth) {
        return right;
    }
    return std::max(8.0f, parentX - childWidth - gap);
}

inline float preferredChildWidth(const std::vector<ContextMenuItem>& items,
                                 const ContextMenuItem::ChildrenPresentation& presentation,
                                 float screenWidth,
                                 const theme::ThemeMetricTokens& metrics) {
    if (presentation.preferredWidth > 0.0f) {
        return std::min(presentation.preferredWidth,
                        std::max(0.0f, screenWidth - metrics.spacing.section));
    }

    const bool hasCheckColumn = std::any_of(items.begin(), items.end(),
        [](const ContextMenuItem& item) { return item.checkable || item.checked; });
    const float fontSize = metrics.typography.option;
    float contentWidth = 0.0f;
    for (const ContextMenuItem& item : items) {
        const float label = core::TextPrimitive::measureTextWidth(item.text, {}, fontSize);
        const MenuItemSlots slots = menuItemSlots(item, hasCheckColumn, presentation, metrics,
                                                  !item.shortcut.empty(), /*childLevel=*/true);
        contentWidth = std::max(contentWidth, slots.leading + label + slots.trailing +
                                              slots.shortcut + slots.shortcutGap);
    }
    const float padding = metrics.spacing.small * 2.0f;
    return std::clamp(contentWidth + padding,
                      0.0f,
                      std::max(0.0f, screenWidth - metrics.spacing.section));
}

} // namespace context_menu_detail

class ContextMenuBuilder {
public:
    ContextMenuBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    ContextMenuBuilder& open(bool value = true) { open_ = value; return *this; }
    ContextMenuBuilder& bindOpen(eui::Signal<bool>& signal) {
        open(signal.get());
        onOpenChange([&signal](bool value) { signal.set(value); });
        return *this;
    }
    ContextMenuBuilder& screen(float width, float height) { screenWidth_ = width; screenHeight_ = height; return *this; }
    ContextMenuBuilder& position(float x, float y) { x_ = x; y_ = y; return *this; }
    // A menu bar can keep its title strip interactive while a popup is open.
    ContextMenuBuilder& dismissTopInset(float value) { dismissTopInset_ = std::max(0.0f, value); return *this; }
    ContextMenuBuilder& size(float width, float itemHeight) { width_ = width; itemHeight_ = itemHeight; return *this; }
    ContextMenuBuilder& items(std::vector<std::string> value) {
        items_.clear();
        items_.reserve(value.size());
        for (std::string& text : value) {
            items_.emplace_back(std::move(text));
        }
        return *this;
    }
    ContextMenuBuilder& items(std::initializer_list<std::string> value) {
        return items(std::vector<std::string>(value));
    }
    ContextMenuBuilder& items(std::vector<ContextMenuItem> value) { items_ = std::move(value); return *this; }
    ContextMenuBuilder& style(const ContextMenuStyle& value) { style_ = value; return *this; }
    ContextMenuBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = ContextMenuStyle(tokens);
        metrics_ = tokens.metrics;
        return *this;
    }
    ContextMenuBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    ContextMenuBuilder& zIndex(int value) { zIndex_ = value; return *this; }
    ContextMenuBuilder& onSelect(std::function<void(int)> callback) { onSelect_ = std::move(callback); return *this; }
    ContextMenuBuilder& onSelectPath(std::function<void(const std::vector<int>&)> callback) { onSelectPath_ = std::move(callback); return *this; }
    ContextMenuBuilder& onOpenChange(std::function<void(bool)> callback) { onOpenChange_ = std::move(callback); return *this; }
    // 菜单开着时在菜单外**右键**：不关闭，把落点交给应用 —— 用于"右键重新触发"
    // （刷新菜单位置，Windows 桌面的交互惯例）。左键仍是关闭。
    ContextMenuBuilder& onOutsideContextMenu(std::function<void(float, float)> callback) {
        onOutsideContextMenu_ = std::move(callback);
        return *this;
    }

    void build() {
        if (items_.empty()) {
            return;
        }

        const float inset = metrics_.spacing.small;
        const float itemHeight = resolvedItemHeight();
        const float separatorGap = metrics_.spacing.tiny * 2.0f;
        const float width = std::min(width_, std::max(0.0f, screenWidth_ - metrics_.spacing.section));
        const float xInset = metrics_.spacing.compact;
        const float height = context_menu_detail::menuHeight(items_, itemHeight, inset, separatorGap);
        const float x = std::clamp(x_, xInset, std::max(xInset, screenWidth_ - width - xInset));
        const float y = clampVisibleMenuY(y_, height);
        const std::function<void()> requestDismiss = dismissCallback();
        const std::function<void(int)> onSelect = onSelect_;
        const std::function<void(const std::vector<int>&)> onSelectPath = onSelectPath_;
        const std::function<void(float, float)> onOutsideContextMenu = onOutsideContextMenu_;
        CascadeState* cascade = &ui_.state<CascadeState>(id_ + ".cascade");
        if (!open_) {
            cascade->openPath.clear();
            cascade->renderedPath.clear();
            cascade->hoveredPath.clear();
            cascade->pressedPath.clear();
        }

        ui_.stack(id_)
            .size(screenWidth_, screenHeight_)
            .zIndex(zIndex_)
            .content([&] {
                if (open_) {
                    ui_.rect(id_ + ".dismiss")
                        .position(0.0f, dismissTopInset_)
                        .size(screenWidth_, std::max(0.0f, screenHeight_ - dismissTopInset_))
                        .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                                theme::color(0.0f, 0.0f, 0.0f, 0.0f))
                        .onClick(requestDismiss)
                        .onContextMenu([onOutsideContextMenu](const core::PointerEvent& event, const core::Rect&) {
                            // 右键落到菜单外：交给应用刷新菜单位置（不关闭）。
                            if (onOutsideContextMenu) {
                                onOutsideContextMenu(static_cast<float>(event.x), static_cast<float>(event.y));
                            }
                        })
                        .onScroll([](const core::ScrollEvent&) {})
                        .build();
                }

                ui_.stack(id_ + ".menu")
                    .size(screenWidth_, screenHeight_)
                    .content([&] {
                        renderMenus(x, y, width, inset, *cascade, onSelect, onSelectPath, requestDismiss);
                    })
                    .build();
            })
            .build();
    }

private:
    float visibleMenuHeight(float height) const {
        return std::min(height, std::max(1.0f, screenHeight_ - std::max(8.0f, dismissTopInset_) - 8.0f));
    }
    float clampVisibleMenuY(float desiredY, float height) const {
        return std::max(dismissTopInset_, context_menu_detail::clampMenuY(
            desiredY, visibleMenuHeight(height), screenHeight_));
    }
    struct CascadeState {
        std::vector<int> openPath;
        std::vector<int> renderedPath;
        std::vector<int> hoveredPath;
        std::vector<int> pressedPath;
    };

    float resolvedItemHeight() const {
        return itemHeight_ > 0.0f ? itemHeight_ : metrics_.control.segmented;
    }

    // 子级宽度：preferred>0 用请求值，否则继承父级已解析宽度；两者都再受屏宽安全边界约束。
    float resolvedLevelWidth(float preferred, float inherited) const {
        const float requested = preferred > 0.0f ? preferred : inherited;
        return std::min(requested, std::max(0.0f, screenWidth_ - metrics_.spacing.section));
    }

    static int maximumDepth(const std::vector<ContextMenuItem>& items) {
        int depth = 1;
        for (const ContextMenuItem& item : items) {
            if (item.hasChildren()) {
                depth = std::max(depth, 1 + maximumDepth(item.children));
            }
        }
        return depth;
    }

    void renderMenus(float rootX, float rootY, float width, float inset, CascadeState& cascade,
                     const std::function<void(int)>& onSelect,
                     const std::function<void(const std::vector<int>&)>& onSelectPath,
                     const std::function<void()>& requestDismiss) {
        const std::vector<ContextMenuItem>* levelItems = &items_;
        std::vector<int> prefix;
        float menuX = rootX;
        float menuY = rootY;
        const int depthCount = maximumDepth(items_);
        const float itemHeight = resolvedItemHeight();
        const float separatorGap = metrics_.spacing.tiny * 2.0f;
        int renderedLevels = 0;

        // 每一级的宽度/呈现配置来自它的**父项**：根级用调用方的 width/inset 与默认配置，
        // 之后的每一级在进入时从父项取 childrenPresentation 解析自己的宽度与局部列参数。
        float levelWidth = width;
        float levelInset = inset;
        ContextMenuItem::ChildrenPresentation levelPresentation;

        for (int depth = 0; depth < depthCount; ++depth) {
            if (depth > 0) {
                if (depth > static_cast<int>(cascade.renderedPath.size())) {
                    break;
                }
                const int parentIndex = cascade.renderedPath[depth - 1];
                if (parentIndex < 0 || parentIndex >= static_cast<int>(levelItems->size()) ||
                    !(*levelItems)[parentIndex].hasChildren()) {
                    break;
                }
                prefix.push_back(parentIndex);
                const ContextMenuItem& parent = (*levelItems)[static_cast<std::size_t>(parentIndex)];
                const float parentOffset = ui_.state<float>(id_ + ".level." + std::to_string(depth - 1) + ".offset");
                const float desiredY = menuY + context_menu_detail::menuItemTop(
                    *levelItems, static_cast<std::size_t>(parentIndex), itemHeight, levelInset, separatorGap) - parentOffset;
                levelItems = &parent.children;
                levelPresentation = parent.childrenPresentation;
                const float childWidth = levelPresentation.preferredWidth > 0.0f
                    ? resolvedLevelWidth(levelPresentation.preferredWidth, levelWidth)
                    : context_menu_detail::preferredChildWidth(
                          parent.children, levelPresentation, screenWidth_, metrics_);
                menuX = context_menu_detail::childMenuX(menuX, levelWidth, childWidth, screenWidth_, metrics_.spacing.tiny);
                levelWidth = childWidth;
                menuY = clampVisibleMenuY(desiredY,
                    context_menu_detail::menuHeight(*levelItems, itemHeight, levelInset, separatorGap));
            }

            const bool visible = open_ && (depth == 0 || depth <= static_cast<int>(cascade.openPath.size()));
            ui_.state<float>(id_ + ".level." + std::to_string(depth) + ".resolvedWidth") = levelWidth;
            renderLevel(*levelItems, prefix, depth, menuX, menuY, levelWidth, levelInset, levelPresentation,
                        visible, cascade, separatorGap, onSelect, onSelectPath, requestDismiss);
            renderedLevels = depth + 1;
        }

        for (int depth = renderedLevels; depth < depthCount; ++depth) {
            const float previousWidth = ui_.state<float>(
                id_ + ".level." + std::to_string(depth) + ".resolvedWidth");
            renderHiddenLevel(depth, previousWidth > 0.0f ? previousWidth : width);
        }
    }

    void renderLevel(const std::vector<ContextMenuItem>& levelItems, const std::vector<int>& prefix,
                     int depth, float x, float y, float width, float inset,
                     const ContextMenuItem::ChildrenPresentation& presentation,
                     bool visible, CascadeState& cascade,
                     float separatorGap,
                     const std::function<void(int)>& onSelect,
                     const std::function<void(const std::vector<int>&)>& onSelectPath,
                     const std::function<void()>& requestDismiss) {
        const std::string levelId = id_ + ".level." + std::to_string(depth);
        const float itemHeight = resolvedItemHeight();
        const float height = context_menu_detail::menuHeight(levelItems, itemHeight, inset, separatorGap);
        const float viewportHeight = visibleMenuHeight(height);
        const float maxOffset = std::max(0.0f, height - viewportHeight);
        float& offset = ui_.state<float>(levelId + ".offset");
        auto& previousPrefix = ui_.state<std::vector<int>>(levelId + ".scrollPrefix");
        if (!visible || previousPrefix != prefix) offset = 0.0f;
        previousPrefix = prefix;
        offset = std::clamp(offset, 0.0f, maxOffset);
        // Runtime scroll ownership is identified by the element's own ID.
        const std::string scrollId = levelId + ".viewport";
        const float opacity = visible ? 1.0f : 0.0f;
        const float scale = visible ? 1.0f : 0.94f;
        const float offsetY = visible ? 0.0f : -4.0f;
        const bool hasCheckColumn = std::any_of(levelItems.begin(), levelItems.end(),
            [](const ContextMenuItem& item) { return item.checkable || item.checked; });
        const float checkColumnWidth = presentation.compactCheckColumn ? 24.0f : 26.0f;

        auto level = ui_.stack(levelId);
        level.x(x).y(y).size(width, viewportHeight)
            .disabled(!visible)
            .opacity(opacity)
            .translateY(offsetY)
            .scale(scale)
            .transformOrigin(0.0f, 0.0f);
        if (transition_.enabled) {
            level.transition(transition_)
                .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform);
        }
        level.content([&] {
                ui_.rect(levelId + ".bg").size(width, viewportHeight).color(style_.background)
                    .radius(style_.radius).border(metrics_.spacing.hairline, style_.border).shadow(style_.shadow).build();
                ui_.rect(levelId + ".hit").size(width, viewportHeight)
                    .states(theme::color(0, 0, 0, 0), theme::color(0, 0, 0, 0), theme::color(0, 0, 0, 0))
                    .disabled(!visible).blockPointer().build();

                ui_.stack(levelId + ".viewport").size(width, viewportHeight).clip()
                    .disabled(!visible)
                    .scrollState(scrollId, offset, maxOffset, itemHeight * 2.0f)
                    .onScrollOffsetChanged([&offset](float value) { offset = value; })
                    .content([&] {
                    ui_.stack(levelId + ".content").size(width, height).scrollContentFrom(scrollId).content([&] {

                // One static highlight per menu level prevents independently animated
                // row colors from leaving both the old and new hover rows filled.
                int highlightedIndex = -1;
                bool highlightedPressed = false;
                const auto pathAt = [&prefix](int index) {
                    std::vector<int> path = prefix;
                    path.push_back(index);
                    return path;
                };
                for (int index = 0; index < static_cast<int>(levelItems.size()); ++index) {
                    const std::vector<int> path = pathAt(index);
                    if (cascade.pressedPath == path) {
                        highlightedIndex = index;
                        highlightedPressed = true;
                        break;
                    }
                    if (cascade.hoveredPath == path) {
                        highlightedIndex = index;
                        break;
                    }
                    const bool submenuOpen = levelItems[index].hasChildren() &&
                        cascade.openPath == path;
                    if (submenuOpen) {
                        highlightedIndex = index;
                    }
                }
                if (highlightedIndex >= 0) {
                    const float highlightedY = context_menu_detail::menuItemTop(
                        levelItems, static_cast<std::size_t>(highlightedIndex), itemHeight, inset, separatorGap);
                    ui_.rect(levelId + ".highlight")
                        .x(inset).y(highlightedY)
                        .size(std::max(0.0f, width - inset * 2.0f), itemHeight)
                        .color(highlightedPressed ? style_.pressed : style_.hover)
                        .radius(std::max(metrics_.radius.tiny, style_.radius - metrics_.radius.tiny))
                        .build();
                }

                for (int index = 0; index < static_cast<int>(levelItems.size()); ++index) {
                    const ContextMenuItem& item = levelItems[index];
                    const float itemY = context_menu_detail::menuItemTop(
                        levelItems, static_cast<std::size_t>(index), itemHeight, inset, separatorGap);
                    std::vector<int> path = prefix;
                    path.push_back(index);
                    const float shortcutMeasuredWidth = item.shortcut.empty()
                        ? 0.0f
                        : std::max(80.0f, core::TextPrimitive::measureTextWidth(
                              item.shortcut, {}, metrics_.typography.micro));
                    const context_menu_detail::MenuItemSlots baseSlots = context_menu_detail::menuItemSlots(
                        item, hasCheckColumn, presentation, metrics_, false, depth > 0);
                    const float availableShortcutWidth = std::max(0.0f,
                        width - inset * 2.0f - baseSlots.leading - baseSlots.trailing -
                        metrics_.spacing.content);
                    const bool showShortcut = !item.shortcut.empty() &&
                                              availableShortcutWidth >= shortcutMeasuredWidth;
                    const context_menu_detail::MenuItemSlots slots = context_menu_detail::menuItemSlots(
                        item, hasCheckColumn, presentation, metrics_, showShortcut, depth > 0);
                    const float labelHeight = metrics_.typography.option + metrics_.typography.lineGapTight;
                    if (item.separatorBefore) {
                        ui_.rect(levelId + ".separator." + std::to_string(index))
                            .x(inset + metrics_.spacing.content)
                            .y(itemY - separatorGap + (separatorGap - metrics_.spacing.hairline) * 0.5f)
                            .size(std::max(0.0f, width - inset * 2.0f - metrics_.spacing.content * 2.0f),
                                  metrics_.spacing.hairline)
                            .color(style_.border)
                            .build();
                    }
                    ui_.rect(levelId + ".item." + std::to_string(index))
                        .x(inset).y(itemY).size(std::max(0.0f, width - inset * 2.0f), itemHeight)
                        .color(theme::color(0, 0, 0, 0))
                        .cursor(item.enabled ? core::CursorShape::Hand : core::CursorShape::Arrow)
                        .disabled(!visible)
                        .onHover([cascadePtr = &cascade, path, prefix, hasChildren = item.hasChildren(),
                                  enabled = item.enabled](bool isHovered) {
                            if (!isHovered) {
                                if (cascadePtr->hoveredPath == path) cascadePtr->hoveredPath.clear();
                                if (cascadePtr->pressedPath == path) cascadePtr->pressedPath.clear();
                                return;
                            }
                            if (!enabled) {
                                cascadePtr->hoveredPath.clear();
                                cascadePtr->openPath = prefix;
                                cascadePtr->renderedPath = prefix;
                                return;
                            }
                            cascadePtr->hoveredPath = path;
                            cascadePtr->openPath = path;
                            if (hasChildren) {
                                cascadePtr->renderedPath = path;
                            } else {
                                cascadePtr->openPath.pop_back();
                                cascadePtr->renderedPath = prefix;
                            }
                        })
                        .onPress([cascadePtr = &cascade, path, enabled = item.enabled](const core::PointerEvent&,
                                                                                        const core::Rect&) {
                            if (enabled) cascadePtr->pressedPath = path;
                        })
                        .onRelease([cascadePtr = &cascade, path](const core::PointerEvent&,
                                                                  const core::Rect&) {
                            if (cascadePtr->pressedPath == path) cascadePtr->pressedPath.clear();
                        })
                        .onClick([onSelect, onSelectPath, requestDismiss, path, index,
                                  hasChildren = item.hasChildren(), enabled = item.enabled, cascadePtr = &cascade] {
                            if (!enabled) return;
                            if (hasChildren) {
                                cascadePtr->openPath = path;
                                cascadePtr->renderedPath = path;
                                return;
                            }
                            if (onSelectPath) onSelectPath(path);
                            if (onSelect) onSelect(index);
                            requestDismiss();
                        })
                        .cursor(item.enabled ? core::CursorShape::Hand : core::CursorShape::Arrow)
                        .build();
                    ui_.text(levelId + ".label." + std::to_string(index))
                        .x(inset + slots.leading)
                        .y(itemY + std::max(0.0f, (itemHeight - labelHeight) * 0.5f))
                        .size(std::max(0.0f, width - inset * 2.0f - slots.leading -
                                      slots.trailing - slots.shortcut - slots.shortcutGap),
                              labelHeight)
                        .text(item.text)
                        .fontSize(metrics_.typography.option)
                        .lineHeight(metrics_.typography.option + metrics_.typography.lineGapTight)
                        .horizontalAlign(presentation.centerLabels ? core::HorizontalAlign::Center
                                                                  : core::HorizontalAlign::Left)
                        .color(item.enabled ? style_.text : style_.mutedText).build();
                    if (item.checked) {
                        const float markSize = 16.0f;
                        const float markX = inset + metrics_.spacing.micro +
                            (checkColumnWidth - markSize) * 0.5f;
                        const float markY = itemY + (itemHeight - markSize) * 0.5f;
                        vector_icon::drawCheckmark(
                            ui_, levelId + ".check." + std::to_string(index), markX, markY,
                            markSize, item.enabled ? style_.text : style_.mutedText);
                    }
                    if (showShortcut) {
                        const float shortcutX = width - inset - slots.trailing - slots.shortcut;
                        ui_.text(levelId + ".shortcut." + std::to_string(index))
                            .x(shortcutX)
                            .y(itemY).size(slots.shortcut, itemHeight)
                            .text(item.shortcut)
                            .fontSize(metrics_.typography.micro)
                            .color(style_.mutedText)
                            .horizontalAlign(core::HorizontalAlign::Right)
                            .verticalAlign(core::VerticalAlign::Center)
                            .build();
                    }
                    if (item.hasChildren()) {
                        const float arrowSize = std::min(
                            metrics_.typography.control, itemHeight) * 0.52f;
                        const float arrowX = width - inset - metrics_.control.compact +
                            metrics_.spacing.micro + (metrics_.typography.control - arrowSize) * 0.5f;
                        const float arrowY = itemY + (itemHeight - arrowSize) * 0.5f;
                        vector_icon::drawChevronRight(
                            ui_, levelId + ".arrow." + std::to_string(index), arrowX, arrowY,
                            arrowSize, style_.mutedText);
                    }
                }
                    }).build();
                }).build();
            });
        level.build();
    }

    void renderHiddenLevel(int depth, float width) {
        auto level = ui_.stack(id_ + ".level." + std::to_string(depth));
        level.size(width, 0.0f)
            .opacity(0.0f)
            .translateY(-4.0f)
            .scale(0.94f)
            .transformOrigin(0.0f, 0.0f);
        if (transition_.enabled) {
            level.transition(transition_)
                .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform);
        }
        level.build();
    }

    std::function<void()> dismissCallback() const {
        const std::function<void(bool)> onOpenChange = onOpenChange_;
        return [onOpenChange] {
            if (onOpenChange) {
                onOpenChange(false);
            }
        };
    }

    core::dsl::Ui& ui_;
    std::string id_;
    std::vector<ContextMenuItem> items_;
    ContextMenuStyle style_;
    theme::ThemeMetricTokens metrics_;
    core::Transition transition_ = core::Transition::none();
    std::function<void(int)> onSelect_;
    std::function<void(const std::vector<int>&)> onSelectPath_;
    std::function<void(bool)> onOpenChange_;
    std::function<void(float, float)> onOutsideContextMenu_;
    bool open_ = false;
    float screenWidth_ = 800.0f;
    float screenHeight_ = 600.0f;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float dismissTopInset_ = 0.0f;
    float width_ = 190.0f;
    float itemHeight_ = 0.0f;
    int zIndex_ = 1050;
};

inline ContextMenuBuilder contextMenu(core::dsl::Ui& ui, const std::string& id) {
    return ContextMenuBuilder(ui, id);
}

} // namespace components
