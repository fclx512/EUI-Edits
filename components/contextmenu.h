#pragma once

#include "components/theme.h"
#include "components/text_alignment.h"
#include "components/vector_icon.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <cstdint>
#include <optional>
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
    // Negative values preserve the component's automatic geometry. These are
    // visual-only overrides: action hit rectangles retain their normal inset.
    float highlightInset = -1.0f;
    float highlightRadius = -1.0f;
};

// Optional application motion policy. Unlike transition(), it keeps glyphs at
// their final scale and gives entering, exiting and row feedback separate timing.
struct ContextMenuMotion {
    core::Transition enter;
    core::Transition exit;
    core::Transition highlight;
    float offsetY = -4.0f;
    float offsetX = 0.0f;
    // Keep the final panel/shadow fixed and slide only content inside its
    // existing viewport. No animated layout size or offscreen layer is needed.
    bool clipContent = false;
    bool directionalSubmenus = false;
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

struct FrozenBackdropLifecycle {
    bool wasOpen = false;
    bool wasEnabled = false;
    std::uint64_t session = 0;
};

inline std::uint64_t nextFrozenBackdropSession() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t frozenBackdropPanelKey(const std::string& menuId, int depth,
                                            const std::vector<int>& prefix) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto append = [&hash](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffu);
            hash *= 1099511628211ull;
        }
    };
    append(static_cast<std::uint32_t>(menuId.size()));
    for (unsigned char value : menuId) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    append(static_cast<std::uint32_t>(depth));
    append(static_cast<std::uint32_t>(prefix.size()));
    for (int index : prefix) append(static_cast<std::uint32_t>(index));
    return hash == 0 ? 1 : hash;
}

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
    // Opt in to omitting closed content only when no closing transition is used.
    // The default retains the existing hidden-tree/animation behavior.
    ContextMenuBuilder& skipClosedContent(bool value = true) { skipClosedContent_ = value; return *this; }
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
    // Explicit app opt-in. `enabled=false` still observes open/close edges so
    // enabling the material while a menu is open starts a fresh frozen session.
    ContextMenuBuilder& frozenBackdrop(float sigma = 8.0f, float tintOpacity = 0.78f,
                                       bool enabled = true) {
        frozenBackdropTracked_ = true;
        frozenBackdropEnabled_ = enabled;
        frozenBackdropSigma_ = std::max(0.0f, sigma);
        frozenBackdropTintOpacity_ = std::clamp(tintOpacity, 0.0f, 1.0f);
        return *this;
    }
    // Optional, synchronous decoration between the panel and its hit/row layers.
    // It must preserve layout and avoid interactive descendants; hidden levels
    // do not invoke it. The default leaves existing menus unchanged.
    ContextMenuBuilder& backgroundDecoration(
        std::function<void(core::dsl::Ui&, const std::string&, float, float)> callback) {
        backgroundDecoration_ = std::move(callback);
        return *this;
    }
    ContextMenuBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = ContextMenuStyle(tokens);
        metrics_ = tokens.metrics;
        return *this;
    }
    ContextMenuBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    ContextMenuBuilder& motion(const ContextMenuMotion& value) { motion_ = value; return *this; }
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
        std::uint64_t backdropSession = 0;
        if (frozenBackdropTracked_) {
            auto& lifecycle = ui_.state<context_menu_detail::FrozenBackdropLifecycle>(
                id_ + ".frozenBackdrop.lifecycle");
            if (open_ && frozenBackdropEnabled_ &&
                (!lifecycle.wasOpen || !lifecycle.wasEnabled)) {
                lifecycle.session = context_menu_detail::nextFrozenBackdropSession();
            }
            lifecycle.wasOpen = open_;
            lifecycle.wasEnabled = frozenBackdropEnabled_;
            backdropSession = lifecycle.session;
        }
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
        if (motion_ && open_ && !cascade->wasOpen) {
            cascade->openPath.clear();
            cascade->renderedPath.clear();
            cascade->hoveredPath.clear();
            cascade->pressedPath.clear();
        }
        cascade->wasOpen = open_;
        if (!open_) {
            cascade->openPath.clear();
            if (!retainExitLevels()) cascade->renderedPath.clear();
            cascade->hoveredPath.clear();
            cascade->pressedPath.clear();
            if (skipClosedContent_ && !hasLevelTransitions()) {
                // renderLevel normally resets the root offset while closed.
                ui_.state<float>(id_ + ".level.0.offset") = 0.0f;
                return;
            }
        }

        ui_.stack(id_)
            .size(screenWidth_, screenHeight_)
            .zIndex(zIndex_)
            // A tracked popup remains transient while closing or falling back.
            // Its exit animation must not enter another popup's clean source.
            .excludeFromBackdropSnapshot(frozenBackdropTracked_)
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
                        renderMenus(x, y, width, inset, *cascade, onSelect, onSelectPath,
                                    requestDismiss, backdropSession);
                    })
                    .build();
            })
            .build();
    }

private:
    core::Transition levelTransition(bool visible) const {
        return motion_ ? (visible ? motion_->enter : motion_->exit) : transition_;
    }
    bool hasLevelTransitions() const {
        return motion_ ? motion_->enter.enabled || motion_->exit.enabled : transition_.enabled;
    }
    bool retainExitLevels() const { return motion_ && motion_->exit.enabled; }

    float visibleMenuHeight(float height) const {
        return std::min(height, std::max(1.0f, screenHeight_ - std::max(8.0f, dismissTopInset_) - 8.0f));
    }
    float clampVisibleMenuY(float desiredY, float height) const {
        return std::max(dismissTopInset_, context_menu_detail::clampMenuY(
            desiredY, visibleMenuHeight(height), screenHeight_));
    }
    struct CascadeState {
        bool wasOpen = false;
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
                     const std::function<void()>& requestDismiss,
                     std::uint64_t backdropSession) {
        const std::vector<ContextMenuItem>* levelItems = &items_;
        std::vector<int> prefix;
        float menuX = rootX;
        float menuY = rootY;
        const int depthCount = maximumDepth(items_);
        const float itemHeight = resolvedItemHeight();
        const float separatorGap = metrics_.spacing.tiny * 2.0f;
        int renderedLevels = 0;
        bool opensRight = true;

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
                const float childX = context_menu_detail::childMenuX(menuX, levelWidth, childWidth, screenWidth_, metrics_.spacing.tiny);
                opensRight = childX >= menuX;
                menuX = childX;
                levelWidth = childWidth;
                menuY = clampVisibleMenuY(desiredY,
                    context_menu_detail::menuHeight(*levelItems, itemHeight, levelInset, separatorGap));
            }

            const bool visible = open_ && (depth == 0 || depth <= static_cast<int>(cascade.openPath.size()));
            ui_.state<float>(id_ + ".level." + std::to_string(depth) + ".resolvedWidth") = levelWidth;
            renderLevel(*levelItems, prefix, depth, menuX, menuY, levelWidth, levelInset, levelPresentation,
                        visible, opensRight, cascade, separatorGap, onSelect, onSelectPath, requestDismiss,
                        backdropSession);
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
                     bool visible, bool opensRight, CascadeState& cascade,
                     float separatorGap,
                     const std::function<void(int)>& onSelect,
                     const std::function<void(const std::vector<int>&)>& onSelectPath,
                     const std::function<void()>& requestDismiss,
                     std::uint64_t backdropSession) {
        const std::string levelId = id_ + ".level." + std::to_string(depth);
        const float itemHeight = resolvedItemHeight();
        const float height = context_menu_detail::menuHeight(levelItems, itemHeight, inset, separatorGap);
        const float viewportHeight = visibleMenuHeight(height);
        const float maxOffset = std::max(0.0f, height - viewportHeight);
        float& offset = ui_.state<float>(levelId + ".offset");
        auto& previousPrefix = ui_.state<std::vector<int>>(levelId + ".scrollPrefix");
        bool& wasVisible = ui_.state<bool>(levelId + ".motion.visible");
        if (previousPrefix != prefix || (motion_ ? visible && !wasVisible : !visible)) offset = 0.0f;
        previousPrefix = prefix;
        wasVisible = visible;
        offset = std::clamp(offset, 0.0f, maxOffset);
        // Runtime scroll ownership is identified by the element's own ID.
        const std::string scrollId = levelId + ".viewport";
        const float opacity = visible ? 1.0f : 0.0f;
        const float scale = visible || motion_ ? 1.0f : 0.94f;
        const bool clipContent = motion_ && motion_->clipContent;
        const core::Vec2 hiddenOffset = contentOffset(depth, opensRight);
        const float offsetY = visible || clipContent ? 0.0f : hiddenOffset.y;
        const float offsetX = visible || clipContent ? 0.0f : hiddenOffset.x;
        const bool hasCheckColumn = std::any_of(levelItems.begin(), levelItems.end(),
            [](const ContextMenuItem& item) { return item.checkable || item.checked; });
        const float checkColumnWidth = presentation.compactCheckColumn ? 24.0f : 26.0f;
        const float labelTop = text_alignment::singleLineTop(itemHeight, metrics_.typography.option);
        const float shortcutTop = text_alignment::singleLineTop(itemHeight, metrics_.typography.micro);

        auto level = ui_.stack(levelId);
        level.x(x).y(y).size(width, viewportHeight)
            .disabled(!visible)
            .opacity(opacity)
            .translateX(offsetX)
            .translateY(offsetY)
            .scale(scale)
            .transformOrigin(0.0f, 0.0f);
        const auto transition = levelTransition(visible);
        if (transition.enabled) {
            level.transition(transition)
                .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform);
        }
        if (depth > 0 && !visible && retainExitLevels()) {
            // Keep rows only for their exit. A bounded timer prunes the hidden
            // child tree after the final decoration sample; it is then removed.
            const auto exit = core::scaledTransition(motion_->exit);
            const float delay = exit.delaySeconds + exit.durationSeconds + 1.0f / 60.0f;
            level.onTimer(delay, [cascadePtr = &cascade, depth, prefix] {
                if (static_cast<int>(cascadePtr->openPath.size()) >= depth ||
                    cascadePtr->renderedPath.size() < prefix.size() ||
                    !std::equal(prefix.begin(), prefix.end(), cascadePtr->renderedPath.begin())) return;
                cascadePtr->renderedPath.resize(std::min(cascadePtr->renderedPath.size(),
                                                        static_cast<std::size_t>(depth - 1)));
            });
        }
        level.content([&] {
                auto background = ui_.rect(levelId + ".bg").size(width, viewportHeight).color(style_.background)
                    .radius(style_.radius).border(metrics_.spacing.hairline, style_.border).shadow(style_.shadow);
                if (visible && frozenBackdropEnabled_ && backdropSession != 0) {
                    core::FrozenBackdrop material;
                    material.session = backdropSession;
                    material.panel = context_menu_detail::frozenBackdropPanelKey(id_, depth, prefix);
                    material.sigma = frozenBackdropSigma_;
                    material.tintOpacity = frozenBackdropTintOpacity_;
                    background.frozenBackdrop(material);
                }
                background.build();
                if (visible && backgroundDecoration_) {
                    backgroundDecoration_(ui_, levelId, width, viewportHeight);
                }
                ui_.rect(levelId + ".hit").size(width, viewportHeight)
                    .states(theme::color(0, 0, 0, 0), theme::color(0, 0, 0, 0), theme::color(0, 0, 0, 0))
                    .disabled(!visible).blockPointer().build();

                ui_.stack(levelId + ".viewport").size(width, viewportHeight).clip()
                    .disabled(!visible)
                    .scrollState(scrollId, offset, maxOffset, itemHeight * 2.0f)
                    .onScrollOffsetChanged([&offset](float value) { offset = value; })
                    .content([&] {
                    const auto drawContent = [&] {
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
                if (!visible) highlightedIndex = -1;
                if (motion_ || highlightedIndex >= 0) {
                    struct HighlightState {
                        int index = -1;
                        float y = 0.0f, width = 0.0f, offset = 0.0f;
                        std::vector<int> prefix;
                        bool follow = false;
                    };
                    auto& previous = ui_.state<HighlightState>(levelId + ".motion.highlight");
                    const float highlightedY = highlightedIndex >= 0 ? context_menu_detail::menuItemTop(
                        levelItems, static_cast<std::size_t>(highlightedIndex), itemHeight, inset, separatorGap)
                        : previous.y;
                    const bool follow = visible && highlightedIndex >= 0 && previous.index >= 0 &&
                        (std::abs(highlightedIndex - previous.index) == 1 ||
                         (highlightedIndex == previous.index && previous.follow)) && previous.prefix == prefix &&
                        core::closeEnough(previous.width, width) && core::closeEnough(previous.offset, offset);
                    const float highlightInset = style_.highlightInset >= 0.0f ? style_.highlightInset : inset;
                    const float highlightRadius = style_.highlightRadius >= 0.0f ? style_.highlightRadius
                        : std::max(metrics_.radius.tiny, style_.radius - metrics_.radius.tiny);
                    auto highlight = ui_.rect(levelId + ".highlight")
                        .x(highlightInset).y(highlightedY)
                        .size(std::max(0.0f, width - highlightInset * 2.0f), itemHeight)
                        .opacity(highlightedIndex >= 0 ? 1.0f : 0.0f)
                        .color(highlightedPressed ? style_.pressed : style_.hover)
                        .radius(highlightRadius)
                        .hitTestMode(core::dsl::HitTestMode::None);
                    if (motion_ && highlightedIndex >= 0 && motion_->highlight.enabled) {
                        highlight.transition(motion_->highlight).animate(core::AnimProperty::Color |
                            (follow ? core::AnimProperty::Frame : core::AnimProperty::None));
                    }
                    highlight.build();
                    previous = {highlightedIndex, highlightedY, width, offset, prefix, follow};
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
                                  enabled = item.enabled, retainExit = retainExitLevels()](bool isHovered) {
                            if (!isHovered) {
                                if (cascadePtr->hoveredPath == path) cascadePtr->hoveredPath.clear();
                                if (cascadePtr->pressedPath == path) cascadePtr->pressedPath.clear();
                                return;
                            }
                            if (!enabled) {
                                cascadePtr->hoveredPath.clear();
                                cascadePtr->openPath = prefix;
                                if (!retainExit) cascadePtr->renderedPath = prefix;
                                return;
                            }
                            cascadePtr->hoveredPath = path;
                            cascadePtr->openPath = path;
                            if (hasChildren) {
                                cascadePtr->renderedPath = path;
                            } else {
                                cascadePtr->openPath.pop_back();
                                if (!retainExit) cascadePtr->renderedPath = prefix;
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
                        .y(itemY + labelTop)
                        .size(std::max(0.0f, width - inset * 2.0f - slots.leading -
                                      slots.trailing - slots.shortcut - slots.shortcutGap),
                              labelHeight)
                        .text(item.text)
                        .fontSize(metrics_.typography.option)
                        .lineHeight(metrics_.typography.option + metrics_.typography.lineGapTight)
                        .horizontalAlign(presentation.centerLabels ? core::HorizontalAlign::Center
                                                                  : core::HorizontalAlign::Left)
                        .verticalAlign(core::VerticalAlign::Top)
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
                            .y(itemY + shortcutTop).size(slots.shortcut, metrics_.typography.micro * 1.2f)
                            .text(item.shortcut)
                            .fontSize(metrics_.typography.micro)
                            .color(style_.mutedText)
                            .horizontalAlign(core::HorizontalAlign::Right)
                            .verticalAlign(core::VerticalAlign::Top)
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
                    };
                    if (clipContent) {
                        auto reveal = ui_.stack(contentMotionId(levelId, depth, opensRight))
                            .size(width, height)
                            .translateX(visible ? 0.0f : hiddenOffset.x)
                            .translateY(visible ? 0.0f : hiddenOffset.y);
                        if (transition.enabled) reveal.transition(transition).animate(core::AnimProperty::Transform);
                        reveal.content(drawContent).build();
                        // Both directions have a primed empty instance before a
                        // submenu first opens. Only the chosen branch has rows.
                        // There are O(depth) inert seeds, never per-row timers.
                        if (depth > 0 && motion_->directionalSubmenus)
                            seedContentMotion(levelId, depth, !opensRight, width);
                    } else {
                        drawContent();
                    }
                }).build();
            });
        level.build();
    }

    core::Vec2 contentOffset(int depth, bool opensRight) const {
        if (!motion_) return {0.0f, -4.0f};
        if (depth > 0 && motion_->directionalSubmenus) {
            const float distance = std::abs(motion_->offsetX) > 0.0f
                ? std::abs(motion_->offsetX) : std::abs(motion_->offsetY);
            return {opensRight ? -distance : distance, 0.0f};
        }
        return {motion_->offsetX, motion_->offsetY};
    }

    std::string contentMotionId(const std::string& levelId, int depth, bool opensRight) const {
        return levelId + (depth > 0 && motion_->directionalSubmenus
            ? (opensRight ? ".reveal.right" : ".reveal.left") : ".reveal.root");
    }

    void seedContentMotion(const std::string& levelId, int depth, bool opensRight, float width) {
        const core::Vec2 offset = contentOffset(depth, opensRight);
        ui_.stack(contentMotionId(levelId, depth, opensRight)).size(width, 0.0f)
            .translateX(offset.x).translateY(offset.y).disabled()
            .hitTestMode(core::dsl::HitTestMode::None).build();
    }

    void renderHiddenLevel(int depth, float width) {
        const std::string levelId = id_ + ".level." + std::to_string(depth);
        auto level = ui_.stack(levelId);
        level.size(width, 0.0f)
            .disabled()
            .opacity(0.0f)
            .translateX(motion_ && !motion_->clipContent ? motion_->offsetX : 0.0f)
            .translateY(motion_ && motion_->clipContent ? 0.0f : motion_ ? motion_->offsetY : -4.0f)
            .scale(motion_ ? 1.0f : 0.94f)
            .transformOrigin(0.0f, 0.0f);
        const auto transition = levelTransition(false);
        if (transition.enabled) {
            level.transition(transition)
                .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform);
        }
        if (motion_ && motion_->clipContent) level.content([&] {
            seedContentMotion(levelId, depth, true, width);
            if (depth > 0 && motion_->directionalSubmenus)
                seedContentMotion(levelId, depth, false, width);
        });
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
    std::function<void(core::dsl::Ui&, const std::string&, float, float)> backgroundDecoration_;
    bool frozenBackdropTracked_ = false;
    bool frozenBackdropEnabled_ = false;
    float frozenBackdropSigma_ = 8.0f;
    float frozenBackdropTintOpacity_ = 0.78f;
    theme::ThemeMetricTokens metrics_;
    core::Transition transition_ = core::Transition::none();
    std::optional<ContextMenuMotion> motion_;
    std::function<void(int)> onSelect_;
    std::function<void(const std::vector<int>&)> onSelectPath_;
    std::function<void(bool)> onOpenChange_;
    std::function<void(float, float)> onOutsideContextMenu_;
    bool open_ = false;
    bool skipClosedContent_ = false;
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
