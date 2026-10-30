#pragma once
#include "platform/window_chrome.h"

#include "state/app_actions.h"
#include "model/tab_presentation.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neo {
namespace tab_bar_detail {

// ── 顶栏固定几何（DIP）；公式本体在 model/tab_presentation.h 的 layout 命名空间，
// 绘制与单测共用同一套，避免两处漂移。────────────────────────────────────────
inline constexpr float kTabWidth = tabpresentation::layout::tabWidth;         // 176
inline constexpr float kTabGap = tabpresentation::layout::tabGap;             // 0
inline constexpr float kTabPitch = tabpresentation::layout::tabPitch;         // 176
inline constexpr float kTabCloseSize = 24.0f;                                 // 关闭 hit
inline constexpr float kTabCloseCenter = tabpresentation::layout::closeCenterInset;  // 162
inline constexpr float kDragThreshold = 6.0f;

struct TabActionsLayout {
    static constexpr float width = 28.0f;
    bool showPin = false, showList = false, showNew = false;
    float pinX = 0, listX = 0, newX = 0;
};
inline TabActionsLayout tabActionsLayout(float left, float right) {
    TabActionsLayout layout;
    const float available = std::max(0.0f, right - left);
    layout.showPin = available >= layout.width;
    layout.showList = available >= layout.width * 2;
    layout.showNew = available >= layout.width * 3 + 20;
    layout.pinX = right - (layout.showPin ? layout.width : 0);
    layout.listX = layout.pinX - (layout.showList ? layout.width : 0);
    layout.newX = layout.listX - (layout.showNew ? layout.width : 0);
    return layout;
}

inline void pinButtonView(eui::Ui& ui, const EditorColors& colors, float x, float height, bool interactive) {
    const bool pinned = platform::chrome::alwaysOnTop();
    platform::chrome::exclude(x + 1, 4, 26, height - 8);
    ui.rect("menubar.tabs.pin.hit").position(x + 1, 4).size(26, height - 8).radius(neo::corner(colors, 7.0f))
        .states(pinned ? core::mixColor(colors.editor, colors.accent, .18f) : transparentColor(),
                colors.rowHover, colors.pressed)
        .preserveFocusOnPress().disabled(!interactive)
        .onClick([interactive] {
            if (!interactive) return;
            platform::chrome::setAlwaysOnTop(!platform::chrome::alwaysOnTop());
            app::requestUpdate();
        }).onHover([&ui](bool hovered) {
            ui.state<bool>("menubar.tabs.pin.hover") = hovered;
            app::requestUpdate();
        }).build();
    iconView(ui, "menubar.tabs.pin.icon", UiIcon::Pin, x + 5, (height - 18) * .5f, 18,
             pinned ? colors.accent : colors.textMuted);
    if (ui.state<bool>("menubar.tabs.pin.hover")) {
        const std::string label = i18n::tr(pinned ? "window.unpin" : "window.pin");
        const float width = std::max(60.0f, core::TextPrimitive::measureTextWidth(label, kUiFontFamily, 12) + 20);
        ui.stack("menubar.tabs.pin.tip").position(x + 28 - width, height + 4).size(width, 28)
            .zIndex(2800).hitTestMode(core::dsl::HitTestMode::None).content([&] {
                ui.rect("menubar.tabs.pin.tip.bg").fill().radius(neo::corner(colors, 5.0f)).color(colors.panel)
                    .border(1, colors.border).build();
                ui.text("menubar.tabs.pin.tip.label").fill()
                    .text(label)
                    .fontSize(12).color(colors.text).horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center).build();
            }).build();
    }
}

struct DragState {
    std::uint64_t source = 0;
    std::uint64_t activeAtStart = 0;
    std::vector<std::uint64_t> originalOrder;
    bool dragging = false;
    float pointerX = 0.0f;
    float grabOffset = 0.0f;
    std::size_t targetIndex = 0;
};

inline bool dragInProgress = false;
inline bool escapeCancelRequested = false;

inline bool requestDragCancelOnEscape() {
    if (!dragInProgress) return false;
    escapeCancelRequested = true;
    return true;
}

inline void cancelDrag(eui::Ui& ui) {
    auto& drag = ui.state<DragState>("menubar.tabs.drag");
    if (drag.source != 0)
        ui.state<std::uint64_t>("menubar.tabs.drag.suppressedRelease") = drag.source;
    drag = {};
    ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
    dragInProgress = false;
    escapeCancelRequested = false;
    platform::chrome::armTabDragEscape(false);
}

inline std::size_t dragTargetIndex(float pointerX, float grabOffset, float left,
                                   float scroll, std::size_t count) {
    if (count == 0) return 0;
    const float contentX = pointerX - grabOffset - left + scroll + kTabWidth * 0.5f;
    return std::min(count - 1, static_cast<std::size_t>(std::max(0.0f,
        std::floor(contentX / kTabPitch))));
}

inline std::string basename(const std::string& path) {
    if (path.empty()) return {};
    const std::size_t slash = path.find_last_of("/\\");
    return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

inline std::string parentName(const std::string& path) {
    if (path.empty()) return {};
    const std::size_t last = path.find_last_of("/\\");
    if (last == std::string::npos || last == 0) return {};
    const std::size_t previous = path.find_last_of("/\\", last - 1);
    return path.substr(previous == std::string::npos ? 0 : previous + 1,
                       last - (previous == std::string::npos ? 0 : previous + 1));
}

inline std::string lowerAscii(std::string value) {
    for (char& c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') c = static_cast<char>(byte + ('a' - 'A'));
        if (c == '/') c = '\\';
    }
    return value;
}

inline std::string rootName(const std::string& path) {
    std::string normalized = path;
    while (normalized.size() > 1 && (normalized.back() == '/' || normalized.back() == '\\')) {
        normalized.pop_back();
    }
    return basename(normalized);
}

// Built from the current TabInfo snapshot only; names, roots and translations
// cannot survive a frame here. The two keys match the legacy conflict scans.
struct TabLabelIndex {
    std::unordered_map<std::string, int> names;
    std::unordered_map<std::string, int> parentLabels;
    explicit TabLabelIndex(const std::vector<TabInfo>& tabs) {
        names.reserve(tabs.size());
        parentLabels.reserve(tabs.size());
        for (const auto& tab : tabs) {
            if (tab.path.empty()) continue;
            ++names[lowerAscii(basename(tab.path))];
            const auto base = tab.name.empty() ? std::string(i18n::tr("tabs.untitled")) : tab.name;
            ++parentLabels[lowerAscii(parentName(tab.path) + " / " + base)];
        }
    }
    static int count(const std::unordered_map<std::string, int>& values, const std::string& key) {
        const auto found = values.find(key);
        return found == values.end() ? 0 : found->second;
    }
};

inline std::string tabLabel(const TabInfo& tab, const std::vector<TabInfo>& tabs,
                            const TabLabelIndex* index = nullptr) {
    const std::string base = tab.name.empty() ? std::string(i18n::tr("tabs.untitled")) : tab.name;
    if (tab.path.empty()) return base;
    const std::string key = lowerAscii(basename(tab.path));
    int sameName = index ? TabLabelIndex::count(index->names, key) : 0;
    if (!index) for (const TabInfo& other : tabs) {
        if (!other.path.empty() && lowerAscii(basename(other.path)) == key && ++sameName == 2) break;
    }
    if (sameName < 2) return base;

    std::string parent = parentName(tab.path);
    if (parent.empty()) parent = rootName(tab.vaultRoot);
    std::string label = parent.empty() ? base : parent + " / " + base;
    const auto labelKey = lowerAscii(label);
    int sameLabel = index ? TabLabelIndex::count(index->parentLabels, labelKey) : 0;
    if (!index) for (const TabInfo& other : tabs) {
        const std::string otherBase = other.name.empty() ? std::string(i18n::tr("tabs.untitled")) : other.name;
        if (!other.path.empty() && lowerAscii(parentName(other.path) + " / " + otherBase) == labelKey && ++sameLabel == 2) break;
    }
    if (sameLabel > 1) {
        const std::string root = rootName(tab.vaultRoot);
        if (!root.empty()) label = root + " / " + label;
    }
    return label;
}

inline UiIcon tabIcon(const std::string& path, const std::string& language) {
    if (language == "image" || filetypes::image(path)) return UiIcon::ImageFile;
    if (path.empty()) return language == "markdown" ? UiIcon::MarkdownFile : UiIcon::TextFile;
    switch (filetypes::detect(path).category) {
    case filetypes::Category::Markdown: return UiIcon::MarkdownFile;
    case filetypes::Category::Text: return UiIcon::TextFile;
    case filetypes::Category::Code: return UiIcon::CodeFile;
    case filetypes::Category::Data: return UiIcon::DataFile;
    default: return UiIcon::File;
    }
}

// 库色统一从展示分组缓存取（同根三处入口一致）；无库根用中性 textMuted。
inline eui::Color groupColor(const EditorColors& colors, const TabInfo& tab) {
    if (!tab.hasGroup) return colors.textMuted;
    const tabpresentation::Rgb value = tabpresentation::paletteColor(colors.tokens.dark, tab.colorSlot);
    return eui::Color(value.r, value.g, value.b, 1.0f);
}

inline eui::Color groupColor(const EditorColors& colors, bool hasGroup, int slot) {
    if (!hasGroup) return colors.textMuted;
    const tabpresentation::Rgb value = tabpresentation::paletteColor(colors.tokens.dark, slot);
    return eui::Color(value.r, value.g, value.b, 1.0f);
}

inline std::string displayedRoot(const AppState& state, const TabInfo& tab) {
    const std::string root = tab.vaultRoot.empty() ? std::string(i18n::tr("tabs.no_vault")) : tab.vaultRoot;
    if (tab.hasGroup && state.tabColorRegistry.groupCount() > tabpresentation::kSlotCount) {
        return std::string(i18n::tr("tabs.vault")) + " " + std::to_string(tab.groupOrdinal) + " · " + root;
    }
    return root;
}

// ── 实测省略（本视图专用）─────────────────────────────────────────────────
// 旧 elideToWidth 是 ASCII 估算；这里用真实字形测量，按 UTF-8 codepoint 边界切分，
// 不切断中文/emoji/组合字符。只在需要省略时做 O(log n) 次测量。
inline std::size_t utf8LeadLength(unsigned char lead) {
    if (lead < 0x80u) return 1;
    if ((lead >> 5) == 0x6u) return 2;
    if ((lead >> 4) == 0xEu) return 3;
    if ((lead >> 3) == 0x1Eu) return 4;
    return 1;
}

inline bool combiningCodepointAt(const std::string& text, std::size_t index) {
    if (index >= text.size()) return false;
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    std::uint32_t code = lead;
    const std::size_t length = utf8LeadLength(lead);
    if (length == 2 && index + 1 < text.size()) {
        code = (static_cast<std::uint32_t>(lead & 0x1Fu) << 6) |
               (static_cast<unsigned char>(text[index + 1]) & 0x3Fu);
    } else if (length == 3 && index + 2 < text.size()) {
        code = (static_cast<std::uint32_t>(lead & 0x0Fu) << 12) |
               ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[index + 2]) & 0x3Fu);
    } else if (length == 4 && index + 3 < text.size()) {
        code = (static_cast<std::uint32_t>(lead & 0x07u) << 18) |
               ((static_cast<unsigned char>(text[index + 1]) & 0x3Fu) << 12) |
               ((static_cast<unsigned char>(text[index + 2]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[index + 3]) & 0x3Fu);
    }
    return (code >= 0x0300u && code <= 0x036Fu) || (code >= 0x1AB0u && code <= 0x1AFFu) ||
           (code >= 0x1DC0u && code <= 0x1DFFu) || (code >= 0x20D0u && code <= 0x20F0u) ||
           (code >= 0xFE20u && code <= 0xFE2Fu);
}

inline float measuredWidth(const std::string& text, const std::string& family, float fontSize) {
    if (text.empty()) return 0.0f;
    return core::TextPrimitive::measureTextWidth(text, family, fontSize);
}

inline std::string elideMeasured(const std::string& text, const std::string& family,
                                 float fontSize, float maxWidth) {
    if (maxWidth <= 0.0f || text.empty()) return {};
    if (measuredWidth(text, family, fontSize) <= maxWidth) return text;
    const std::string ellipsis = "\xE2\x80\xA6";
    const float ellipsisWidth = measuredWidth(ellipsis, family, fontSize);
    if (maxWidth <= ellipsisWidth) return ellipsis;
    std::vector<std::size_t> bounds;
    for (std::size_t index = 0; index < text.size();) {
        bounds.push_back(index);
        index += utf8LeadLength(static_cast<unsigned char>(text[index]));
        if (index > text.size()) break;
    }
    bounds.push_back(text.size());
    if (bounds.size() < 2) return text;
    std::size_t low = 1, high = bounds.size() - 2, best = 0;
    while (low <= high) {
        const std::size_t mid = (low + high) / 2;
        const float width = measuredWidth(text.substr(0, bounds[mid]), family, fontSize);
        if (width + ellipsisWidth <= maxWidth) { best = mid; low = mid + 1; }
        else { if (mid == 0) break; high = mid - 1; }
    }
    while (best > 0 && combiningCodepointAt(text, bounds[best])) --best;
    return text.substr(0, bounds[best]) + ellipsis;
}

inline float wrappedHeight(const std::string& text, const std::string& family,
                           float fontSize, float width) {
    core::TextStyle style;
    style.text = text; style.fontFamily = family; style.fontSize = fontSize;
    style.maxWidth = width; style.wrap = true; style.lineHeight = fontSize * 1.25f;
    return std::max(style.lineHeight, core::TextPrimitive::measureTextSize(style).y);
}

inline bool tabSwitchEnabled(const AppState& state) {
    return !state.settingsOpen && !documentModalOpen(state);
}

inline void clearCloseAnchor(AppState& state) {
    clearTabCloseAnchor(state);
}

inline void activate(eui::Ui& ui, AppState& state, std::uint64_t id) {
    if (!tabSwitchEnabled(state)) return;
    clearCloseAnchor(state);
    const bool closeList = state.tabListOpen;
    state.tabListOpen = false;
    const bool changed = activateDocumentTab(state, id);
    ui.requestFocus(editorInputId(state.tabId) + ".hit");
    if (changed || closeList || state.closeAnchorRevealPending) {
        app::requestUpdate();
    }
}

// 顶栏关闭按钮命中：干净页先记录固定指针锚点，再走既有 requestCloseTab 事务；
// 脏页直接清掉关闭链（确认弹窗不自动续关）。activeOnly 窄窗不建立锚点。
inline void closeTabFromBar(AppState& state, std::uint64_t id, bool allowAnchor,
                            float anchorX, float anchorY) {
    auto* page = documentTab(state, id);
    if (!page) return;
    const bool dirty = page->dirty();  // 从模型现读，不用上一帧捕获的 TabInfo.dirty
    if (dirty || !allowAnchor) {
        if (dirty) {
            clearCloseAnchor(state);
        }
        requestCloseTab(state, id);
        app::requestUpdate();
        return;
    }
    AppState::CloseAnchor anchor;
    anchor.enabled = true;
    anchor.anchorX = anchorX;
    anchor.anchorY = anchorY;
    anchor.closedTabId = id;
    anchor.beforeTabOrder = state.tabOrder;
    const auto found = std::find(state.tabOrder.begin(), state.tabOrder.end(), id);
    if (found != state.tabOrder.end()) {
        const auto after = found + 1;
        if (after != state.tabOrder.end()) anchor.expectedNextId = *after;
        else if (found != state.tabOrder.begin()) anchor.expectedNextId = *(found - 1);
    }
    anchor.drawOffset = 0.0f;
    state.closeAnchor = anchor;
    state.closeAnchorPendingApply = true;
    requestCloseTab(state, id);
    app::requestUpdate();
}

// ── 顶栏标签卡 ────────────────────────────────────────────────────────────
inline void tabCard(eui::Ui& ui, AppState& state, const EditorColors& colors,
                    const UiMetrics& metrics, const TabInfo& tab,
                    const std::vector<TabInfo>& tabs, float x, float width,
                    bool active, bool interactive, bool allowAnchor,
                    const TabLabelIndex* labels = nullptr,
                    float regionLeft = 0.0f, float regionWidth = 0.0f,
                    float maxScroll = 0.0f, bool dragEnabled = false,
                    bool placeholder = false, bool flatLeft = false) {
    const std::string id = "menubar.tabs." + std::to_string(tab.id);
    const float height = metrics.menuBarHeight - 4.0f;
    const float y = 4.0f;
    const float closeWidth = 24.0f;
    const float iconSize = std::min(16.0f, std::max(0.0f, height - 8.0f));
    const float closeHitX = x + std::max(0.0f, width - closeWidth - 2.0f);
    const float closeCenterX = closeHitX + closeWidth * 0.5f;

    // 窄窗按可用宽依次取舍：先保证 24 的关闭 hit，再补图标，再补标题与脏点槽。
    const bool showClose = width >= closeWidth;
    const float iconReserve = 8.0f + iconSize + 7.0f;
    const bool showIcon = width >= iconReserve + closeWidth + 6.0f;
    const float titleLeft = showIcon ? x + iconReserve : x + 8.0f;
    const float dirtySlot = 9.0f;  // 无论 dirty 与否都预留，保存时标题宽不跳动
    const float titleRight = x + width - (showClose ? closeWidth + 8.0f : 8.0f) - dirtySlot;
    const float titleWidth = std::max(0.0f, titleRight - titleLeft);
    const bool showTitle = width >= 56.0f && titleWidth >= 16.0f;
    const std::string family = uiFontFamily(state);
    const std::string label = showTitle
        ? elideMeasured(tabLabel(tab, tabs, labels), family, metrics.menuFontSize, titleWidth)
        : std::string{};

    // One closed face, with no bottom stroke. The lower concave shoulders and
    // upper convex corners share endpoints instead of overlapping clipped shapes.
    if (active && !placeholder) {
        const float r = neo::corner(colors, 8.0f);
        constexpr float pi = 3.14159265358979323846f;
        const float topRadius = std::min(r, width * 0.5f);
        std::vector<core::Vec2> face;
        const auto arc = [&](float cx, float cy, float radius, float start, float end) {
            for (int step = 0; step <= 16; ++step) {
                const float angle = start + (end-start) * step / 16.0f;
                face.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
            }
        };
        if (flatLeft) {
            face.push_back({r, height});
        } else {
            arc(0, height-r, r, pi*.5f, 0);
        }
        arc(r+topRadius, topRadius, topRadius, pi, pi*1.5f);
        arc(r+width-topRadius, topRadius, topRadius, pi*1.5f, pi*2);
        arc(width+2*r, height-r, r, pi, pi*.5f);
        // Extend into the editor by one DIP: two separately antialiased edges
        // meeting at fractional device pixels otherwise expose a faint seam.
        face.push_back({width+2*r, height+1.0f});
        face.push_back({flatLeft ? r : 0.0f, height+1.0f});
        ui.polygon(id + ".face").position(x-r, y).size(width+2*r, height+1.0f)
            .points(std::move(face)).color(colors.editor)
            .hitTestMode(core::dsl::HitTestMode::None).build();
    }

    const auto hoverFill = !active && !placeholder &&
        (ui.state<std::uint64_t>("menubar.tabs.hovered") == tab.id ||
         ui.state<std::uint64_t>("menubar.tabs.closeHovered") == tab.id)
        ? core::mixColor(colors.toolbar, colors.editor, 0.62f) : transparentColor();
    if (flatLeft) ui.rect(id + ".squareLeft").position(x, y + height - 8.0f)
        .size(std::min(8.0f, width), 8.0f).color(hoverFill)
        .hitTestMode(core::dsl::HitTestMode::None).build();
    auto surface = ui.rect(id + ".surface");
    surface.position(x, y).size(width, height).radius(neo::corner(colors, 8.0f))
        .color(hoverFill)
        .preserveFocusOnPress().disabled(!interactive)
        .onClick([&ui, &state, idValue = tab.id] {
            const auto& drag = ui.state<DragState>("menubar.tabs.drag");
            if (ui.state<std::uint64_t>("menubar.tabs.drag.suppressedRelease") == idValue) return;
            if (drag.source != idValue || !drag.dragging) activate(ui, state, idValue);
        })
        .transition(quickTransition());
    surface.build();

    // The close slot includes the narrow edge and the space above/below the
    // original close hit. Only the title side participates in tooltip hover.
    ui.rect(id + ".tooltip.hit")
        .position(x, y).size(showClose ? std::max(0.0f, closeHitX - x) : width, height)
        .color(transparentColor()).preserveFocusOnPress().disabled(!interactive)
        .onPress([&ui, &state, idValue = tab.id, cardX = regionLeft + x,
                  dragEnabled](const core::PointerEvent& event, const core::Rect&) {
            if (!dragEnabled || event.button != core::PointerButton::Left) return;
            auto& drag = ui.state<DragState>("menubar.tabs.drag");
            drag = {};
            ui.state<std::uint64_t>("menubar.tabs.drag.suppressedRelease") = 0;
            drag.source = idValue;
            drag.activeAtStart = state.tabId;
            drag.originalOrder = state.tabOrder;
            drag.pointerX = static_cast<float>(event.x);
            drag.grabOffset = drag.pointerX - cardX;
        })
        .onDragUpdate([&ui, &state, idValue = tab.id, dragEnabled,
                       regionLeft](const core::dsl::DragEvent& event) {
            auto& drag = ui.state<DragState>("menubar.tabs.drag");
            if (!dragEnabled || drag.source != idValue || event.button != core::PointerButton::Left)
                return false;
            bool justStarted = false;
            if (!drag.dragging) {
                if (std::fabs(event.totalX) <= kDragThreshold ||
                    std::fabs(event.totalX) <= std::fabs(event.totalY)) return false;
                drag.dragging = true;
                dragInProgress = true;
                platform::chrome::armTabDragEscape(true);
                justStarted = true;
                clearCloseAnchor(state);
                ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
            }
            const bool moved = drag.pointerX != static_cast<float>(event.x);
            drag.pointerX = static_cast<float>(event.x);
            drag.targetIndex = dragTargetIndex(drag.pointerX, drag.grabOffset,
                                               regionLeft, state.tabScroll, state.tabOrder.size());
            return moved || justStarted;
        })
        .onRelease([&ui, &state, idValue = tab.id, interactive, dragEnabled,
                    regionLeft, regionWidth, barHeight = metrics.menuBarHeight]
                   (const core::PointerEvent& event, const core::Rect& bounds) {
            auto& drag = ui.state<DragState>("menubar.tabs.drag");
            auto& suppressed = ui.state<std::uint64_t>("menubar.tabs.drag.suppressedRelease");
            if (suppressed == idValue) {
                if (event.action == core::PointerAction::Release) suppressed = 0;
                return;
            }
            if (event.action == core::PointerAction::Cancel) {
                if (drag.source == idValue) cancelDrag(ui);
                return;
            }
            const bool wasDrag = drag.source == idValue && drag.dragging;
            const std::size_t target = dragTargetIndex(static_cast<float>(event.x),
                drag.grabOffset, regionLeft, state.tabScroll, state.tabOrder.size());
            if (drag.source == idValue) {
                drag = {};
                dragInProgress = false;
                escapeCancelRequested = false;
                platform::chrome::armTabDragEscape(false);
            }
            if (!interactive || event.action != core::PointerAction::Release ||
                event.button != core::PointerButton::Left) return;
            if (wasDrag) {
                ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                if (dragEnabled && event.x >= regionLeft && event.x < regionLeft + regionWidth &&
                    event.y >= 4.0f && event.y < barHeight)
                    moveDocumentTabToIndex(state, idValue, target);
                app::requestUpdate();
            } else if (event.x >= bounds.x && event.x < bounds.x + bounds.width &&
                       event.y >= bounds.y && event.y < bounds.y + bounds.height) {
                activate(ui, state, idValue);
            }
        })
        .onHover([&ui, idValue = tab.id](bool hovered) {
            std::uint64_t& current = ui.state<std::uint64_t>("menubar.tabs.hovered");
            if (ui.state<DragState>("menubar.tabs.drag").dragging) current = 0;
            else if (hovered) current = idValue;
            else if (current == idValue) current = 0;
            app::requestUpdate();
        })
        .build();

    if (placeholder) return;

    ui.rect(id + ".vaultColor")
        .position(x + 3.0f, y + 6.0f).size(3.0f, std::max(3.0f, height - 12.0f))
        .radius(1.5f).color(groupColor(colors, tab)).build();

    if (showIcon) {
        const auto detected = tab.path.empty()
            ? filetypes::detect(tab.language == "markdown" ? "new.md" : "new.txt")
            : filetypes::detect(tab.path);
        const eui::Color iconColor = detected.category == filetypes::Category::Markdown ? colors.iconMd
            : detected.category == filetypes::Category::Text ? colors.iconTxt
            : detected.category == filetypes::Category::Code ? colors.iconCode
            : detected.category == filetypes::Category::Data ? colors.iconData : colors.iconFile;
        iconView(ui, id + ".icon", tabIcon(tab.path, tab.language),
                 x + 8.0f, y + (height - iconSize) * 0.5f, iconSize, iconColor);
    }

    if (showTitle) {
        ui.text(id + ".name")
            .position(titleLeft, y).size(titleWidth, height)
            .text(label).fontFamily(family).fontSize(metrics.menuFontSize)
            .color(active ? colors.text : colors.textMuted)
            .verticalAlign(eui::VerticalAlign::Center).wrap(false).build();
    }

    if (tab.dirty && showTitle) {
        ui.rect(id + ".dirty")
            // titleRight already leaves 15 DIP before the close hit area;
            // use that gap so the dirty marker no longer sits on the ellipsis.
            .position(titleRight + 5.0f, y + (height - 5.0f) * 0.5f)
            .size(5.0f, 5.0f).radius(2.5f).color(colors.accent).build();
    }

    if (showClose) {
        ui.rect(id + ".close.hit")
            .position(closeHitX, y + (height - closeWidth) * 0.5f)
            .size(closeWidth, closeWidth).radius(neo::corner(colors, 6.0f))
            .states(transparentColor(), colors.rowHover, colors.pressed)
            .preserveFocusOnPress().disabled(!interactive)
            .onHover([&ui, idValue = tab.id](bool hovered) {
                auto& current = ui.state<std::uint64_t>("menubar.tabs.closeHovered");
                if (hovered) current = idValue;
                else if (current == idValue) current = 0;
                app::requestUpdate();
            })
            .onClick([&state, idValue = tab.id, interactive, allowAnchor,
                      // x 已是区域局部坐标（外层 clip stack 定位在 regionLeft），
                      // 所以关闭中心本身就是区域局部 X，不能再减一次 regionLeft。
                      offsetX = closeCenterX, offsetY = y + height * 0.5f] {
                if (!interactive) return;
                closeTabFromBar(state, idValue, allowAnchor, offsetX, offsetY);
            })
            .build();
        iconView(ui, id + ".close.icon", UiIcon::Close,
                 closeHitX + 5.0f, y + (height - 14.0f) * 0.5f, 14.0f,
                 active ? colors.text : colors.textMuted);
    }
}

// ── 展开列表：紧凑独立卡片 ────────────────────────────────────────────────
struct ListMetrics {
    float detailFont = 11.0f;
    float detailLine = 14.0f;
    float titleRow = 24.0f;
    float cardHeight = 70.0f;
    float gap = 6.0f;
};

inline ListMetrics listMetrics(const UiMetrics& metrics) {
    ListMetrics result;
    result.detailFont = std::max(10.0f, metrics.menuFontSize - 3.0f);
    result.detailLine = std::ceil(result.detailFont * 1.25f);
    result.titleRow = std::max(24.0f, std::ceil(metrics.menuFontSize * 1.25f));
    result.cardHeight = std::max(70.0f, 16.0f + result.titleRow + 2.0f + 2.0f * result.detailLine);
    return result;
}

inline void tabList(eui::Ui& ui, AppState& state, const eui::Screen& screen,
                    const EditorColors& colors, const UiMetrics& metrics,
                    const std::vector<TabInfo>& tabs, float settingsX, bool interactive) {
    if (!state.tabListOpen || tabs.empty()) {
        // 关闭列表即清空完整信息浮层（删除该 TabId 的清理在下面）。
        ui.state<std::uint64_t>("menubar.tabs.list.infoTab") = 0;
        return;
    }
    const float width = std::min({440.0f, std::max(300.0f, screen.width * 0.42f),
                                  std::max(100.0f, screen.width - 16.0f)});
    const float x = std::clamp(settingsX - width, 8.0f, std::max(8.0f, screen.width - width - 8.0f));
    const float top = metrics.menuBarHeight + 2.0f;
    const ListMetrics lm = listMetrics(metrics);
    const float inset = 8.0f;                                   // 面板四周内边距
    const float cardWidth = std::max(1.0f, width - inset * 2.0f);
    const float contentLeft = 12.0f;                            // 卡内左距（避色条）
    const float contentRight = 10.0f;                           // 卡内右距
    const float actionsWidth = 80.0f;                           // 3×24 + 2×4
    const std::string family = uiFontFamily(state);

    const float contentHeight = static_cast<float>(tabs.size()) * lm.cardHeight +
                                static_cast<float>(tabs.size() > 0 ? tabs.size() - 1 : 0) * lm.gap;
    // 完整信息浮层宽度与标签条 tooltip 同一套自适应规则：按当前悬停项三行文字实测，
    // 下限与上限一致；没有悬停项时才回落到上限宽度。
    const float maxInfoWidth = std::min(480.0f, std::max(220.0f, screen.width - 24.0f));
    float preferredInfoWidth = maxInfoWidth;
    const auto infoSource = std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
        return tab.id == ui.state<std::uint64_t>("menubar.tabs.list.infoTab");
    });
    if (infoSource != tabs.end()) {
        const std::string titleText = infoSource->name.empty()
            ? std::string(i18n::tr("tabs.untitled")) : infoSource->name;
        const std::string pathText = std::string(i18n::tr("tabs.path")) + ": " +
            (infoSource->path.empty() ? std::string(i18n::tr("tabs.unsaved_path")) : infoSource->path);
        const std::string rootText = std::string(i18n::tr("tabs.vault")) + ": " +
            displayedRoot(state, *infoSource);
        const float measuredWidth = std::max({
            core::TextPrimitive::measureTextWidth(titleText, family, lm.detailFont + 1.0f),
            core::TextPrimitive::measureTextWidth(pathText, family, lm.detailFont),
            core::TextPrimitive::measureTextWidth(rootText, family, lm.detailFont)});
        preferredInfoWidth = std::min(maxInfoWidth,
            std::max(std::min(120.0f, maxInfoWidth), measuredWidth + 20.0f));
    }
    const bool infoBelow = x - preferredInfoWidth - 8.0f < 8.0f &&
        x + width + preferredInfoWidth + 16.0f > screen.width;
    float height = std::min({390.0f, inset * 2.0f + contentHeight,
                                   std::max(inset * 2.0f, screen.height - top - 8.0f)});
    // 没有侧向空间且窗口较矮时，给全文浮层保留下面的空间，避免盖住任何行的动作。
    if (infoBelow && ui.state<std::uint64_t>("menubar.tabs.list.infoTab") != 0 &&
        screen.height - top - height - 16.0f < 40.0f) {
        height = std::min(height, std::max(inset * 2.0f, (screen.height - top - 24.0f) * 0.5f));
    }
    const float viewportHeight = std::max(0.0f, height - inset * 2.0f);
    float& scroll = ui.state<float>("menubar.tabs.listScroll");
    std::uint64_t& lastReveal = ui.state<std::uint64_t>("menubar.tabs.listLastReveal");
    const auto rowTop = [&lm](std::size_t index) {
        return static_cast<float>(index) * (lm.cardHeight + lm.gap);
    };
    if (state.tabListReveal != 0 && state.tabListReveal != lastReveal) {
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == state.tabListReveal;
        });
        if (found != tabs.end()) {
            const std::size_t index = static_cast<std::size_t>(std::distance(tabs.begin(), found));
            const float rowTopValue = rowTop(index);
            if (rowTopValue < scroll) scroll = rowTopValue;
            else if (rowTopValue + lm.cardHeight > scroll + viewportHeight) {
                scroll = rowTopValue + lm.cardHeight - viewportHeight;
            }
        }
        lastReveal = state.tabListReveal;
    }
    const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
    scroll = std::clamp(scroll, 0.0f, maxScroll);

    // 悬停行的完整信息浮层：受"离开行/关闭列表/删除该 TabId"清空；指针移入浮层不算离开。
    std::uint64_t& infoTabId = ui.state<std::uint64_t>("menubar.tabs.list.infoTab");
    std::uint64_t& hoveredRow = ui.state<std::uint64_t>("menubar.tabs.list.hovered");
    float& infoX = ui.state<float>("menubar.tabs.list.infoX");
    float& infoY = ui.state<float>("menubar.tabs.list.infoY");
    float& infoW = ui.state<float>("menubar.tabs.list.infoW");
    float& infoH = ui.state<float>("menubar.tabs.list.infoH");
    float& infoRowTop = ui.state<float>("menubar.tabs.list.infoRowTop");
    float& infoRowBottom = ui.state<float>("menubar.tabs.list.infoRowBottom");
    float& infoScroll = ui.state<float>("menubar.tabs.list.infoScroll");
    float& bridgeX = ui.state<float>("menubar.tabs.list.bridgeX");
    float& bridgeY = ui.state<float>("menubar.tabs.list.bridgeY");
    float& bridgeW = ui.state<float>("menubar.tabs.list.bridgeW");
    float& bridgeH = ui.state<float>("menubar.tabs.list.bridgeH");
    float& sideBridgeX = ui.state<float>("menubar.tabs.list.sideBridgeX");
    float& sideBridgeY = ui.state<float>("menubar.tabs.list.sideBridgeY");
    float& sideBridgeW = ui.state<float>("menubar.tabs.list.sideBridgeW");
    float& sideBridgeH = ui.state<float>("menubar.tabs.list.sideBridgeH");
    if (infoTabId != 0 && std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == infoTabId;
        }) == tabs.end()) {
        infoTabId = 0;
    }

    ui.stack("menubar.tabs.list")
        .position(x, top).size(width, height).zIndex(2500)
        .onScroll([&scroll, maxScroll](const core::ScrollEvent& event) {
            const float delta = event.y != 0.0 ? -static_cast<float>(event.y) : static_cast<float>(event.x);
            scroll = std::clamp(scroll + delta * 36.0f, 0.0f, maxScroll);
            app::requestUpdate();
        })
        .content([&] {
            ui.rect("menubar.tabs.list.bg").fill().radius(neo::corner(colors, 10.0f))
                .color(colors.panel).border(1.0f, colors.border)
                .shadow(components::theme::popupShadow(colors.tokens)).build();
            ui.stack("menubar.tabs.list.rows")
                .position(inset, inset).size(width - inset * 2.0f, viewportHeight).clip()
                .content([&] {
                    for (std::size_t index = 0; index < tabs.size(); ++index) {
                        const TabInfo& tab = tabs[index];
                        const float cardTop = rowTop(index) - scroll;
                        if (cardTop + lm.cardHeight < 0.0f || cardTop > viewportHeight) continue;
                        const std::string rowId = "menubar.tabs.list.row." + std::to_string(tab.id);
                        if (infoTabId == tab.id) {
                            infoRowTop = top + inset + cardTop;
                            infoRowBottom = infoRowTop + lm.cardHeight;
                        }
                        const bool active = tab.id == state.tabId;
                        const float titleLeft = contentLeft;
                        const float titleWidth = std::max(0.0f, cardWidth - contentLeft - contentRight -
                                                                    actionsWidth - 8.0f);
                        const std::string title = elideMeasured(
                            tab.name.empty() ? std::string(i18n::tr("tabs.untitled")) : tab.name,
                            family, metrics.menuFontSize, std::max(0.0f, titleWidth - 13.0f));
                        const std::string pathLabel = tab.path.empty()
                            ? std::string(i18n::tr("tabs.unsaved_path")) : tab.path;
                        const std::string rootLabel = displayedRoot(state, tab);
                        const float pathWidth = std::max(0.0f, cardWidth - contentLeft - contentRight);
                        const std::string pathShown = elideMeasured(pathLabel, family, lm.detailFont, pathWidth);
                        const std::string rootShown = elideMeasured(rootLabel, family, lm.detailFont, pathWidth);

                        // 行命中层（在三个动作按钮之下）。
                        ui.rect(rowId + ".hit")
                            .position(0.0f, cardTop).size(cardWidth, lm.cardHeight).radius(neo::corner(colors, 8.0f))
                            .color(active ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.12f)
                                          : colors.toolbar)
                            .border(1.0f, active ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.55f)
                                                 : rgba(colors.border.r, colors.border.g, colors.border.b, 0.66f))
                            .states(active ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.12f)
                                           : colors.toolbar,
                                    colors.rowHover, colors.pressed)
                            .preserveFocusOnPress().disabled(!interactive)
                            .onHover([&hoveredRow, &infoTabId, &infoX, &infoY, &infoW, &infoH,
                                      &infoRowTop, &infoRowBottom, &infoScroll, rowY = top + inset + cardTop,
                                      &bridgeX, &bridgeY, &bridgeW, &bridgeH, rowH = lm.cardHeight,
                                      &sideBridgeX, &sideBridgeY, &sideBridgeW, &sideBridgeH,
                                      idValue = tab.id](bool hovered) {
                                if (hovered) {
                                    hoveredRow = idValue;
                                    if (infoTabId != idValue) infoScroll = 0.0f;
                                    infoTabId = idValue;
                                    infoRowTop = rowY;
                                    infoRowBottom = rowY + rowH;
                                } else if (hoveredRow == idValue) {
                                    hoveredRow = 0;
                                    // 指针从行移入浮层不算离开：用真实光标位置与浮层矩形判断。
                                    float cursorX = 0.0f, cursorY = 0.0f;
                                    const bool insideInfo = infoTabId == idValue &&
                                        cursorClientPosition(cursorX, cursorY) &&
                                        cursorX >= infoX && cursorX <= infoX + infoW &&
                                        cursorY >= infoY && cursorY <= infoY + infoH;
                                    const bool inBridge = bridgeW > 0.0f && bridgeH > 0.0f &&
                                        cursorX >= bridgeX && cursorX <= bridgeX + bridgeW &&
                                        cursorY >= bridgeY && cursorY <= bridgeY + bridgeH;
                                    const bool inSideBridge = sideBridgeW > 0.0f && sideBridgeH > 0.0f &&
                                        cursorX >= sideBridgeX && cursorX <= sideBridgeX + sideBridgeW &&
                                        cursorY >= sideBridgeY && cursorY <= sideBridgeY + sideBridgeH;
                                    if (!insideInfo && !inBridge && !inSideBridge) infoTabId = 0;
                                }
                                app::requestUpdate();
                            })
                            .onClick([&ui, &state, idValue = tab.id, interactive] {
                                if (!interactive) return;
                                activate(ui, state, idValue);
                                state.tabListReveal = idValue;
                                state.tabListOpen = false;
                                ui.requestFocus(editorInputId(state.tabId) + ".hit");
                                app::requestUpdate();
                            })
                            .build();
                        ui.rect(rowId + ".accent")
                            .position(0.0f, cardTop + 8.0f).size(3.0f, std::max(0.0f, lm.cardHeight - 16.0f))
                            .radius(1.5f).color(groupColor(colors, tab)).build();
                        ui.text(rowId + ".name")
                            .position(titleLeft, cardTop + 8.0f).size(titleWidth, lm.titleRow)
                            .text(title).fontFamily(family).fontSize(metrics.menuFontSize)
                            .color(active ? colors.text : colors.textMuted)
                            .verticalAlign(eui::VerticalAlign::Center).wrap(false).build();
                        if (tab.dirty) {
                            ui.rect(rowId + ".dirty")
                                .position(titleLeft + std::max(0.0f, titleWidth - 5.0f),
                                          cardTop + 8.0f + (lm.titleRow - 5.0f) * 0.5f)
                                .size(5.0f, 5.0f).radius(2.5f).color(colors.accent).build();
                        }
                        ui.text(rowId + ".path")
                            .position(titleLeft, cardTop + 8.0f + lm.titleRow + 2.0f)
                            .size(pathWidth, lm.detailLine)
                            .text(pathShown).fontFamily(family).fontSize(lm.detailFont)
                            .color(colors.textMuted).lineHeight(lm.detailLine)
                            .verticalAlign(eui::VerticalAlign::Center).wrap(false).build();
                        ui.text(rowId + ".root")
                            .position(titleLeft, cardTop + 8.0f + lm.titleRow + 2.0f + lm.detailLine)
                            .size(pathWidth, lm.detailLine)
                            .text(rootShown).fontFamily(family).fontSize(lm.detailFont)
                            .color(colors.textMuted).lineHeight(lm.detailLine)
                            .verticalAlign(eui::VerticalAlign::Center).wrap(false).build();

                        // 三个动作：上移 / 下移 / 关闭（各 24×24，间隔 4，总宽 80）。
                        const float actionsX = cardWidth - contentRight - actionsWidth;
                        const float actionsY = cardTop + 8.0f;
                        const bool canUp = index > 0;
                        const bool canDown = index + 1 < tabs.size();
                        const auto actionIcon = [&](const std::string& buttonId, float buttonX,
                                                    UiIcon icon, bool enabled,
                                                    std::function<void()> onClick) {
                            ui.rect(buttonId).position(buttonX, actionsY).size(24.0f, 24.0f)
                                .radius(neo::corner(colors, 5.0f)).states(transparentColor(), colors.rowHover, colors.pressed)
                                .preserveFocusOnPress().disabled(!interactive || !enabled)
                                .onClick([onClick = std::move(onClick), interactive, enabled] {
                                    if (interactive && enabled) onClick();
                                }).build();
                            iconView(ui, buttonId + ".icon", icon, buttonX + 5.0f, actionsY + 5.0f, 14.0f,
                                     enabled ? colors.textMuted
                                             : rgba(colors.textMuted.r, colors.textMuted.g,
                                                    colors.textMuted.b, 0.35f));
                        };
                        actionIcon(rowId + ".up.hit", actionsX, UiIcon::Previous, canUp,
                                   [&state, idValue = tab.id] { moveDocumentTab(state, idValue, -1); app::requestUpdate(); });
                        actionIcon(rowId + ".down.hit", actionsX + 28.0f, UiIcon::Next, canDown,
                                   [&state, idValue = tab.id] { moveDocumentTab(state, idValue, 1); app::requestUpdate(); });
                        actionIcon(rowId + ".close.hit", actionsX + 56.0f, UiIcon::Close, true,
                                   [&state, idValue = tab.id] { requestCloseTab(state, idValue); app::requestUpdate(); });
                    }
                }).build();
        }).build();

    // 完整信息浮层（标题/路径/库根全文）。放在面板左侧优先，屏边则翻到右侧；
    // 与三个动作按钮不重叠；过高时裁剪并可滚轮滚动。
    if (infoTabId != 0) {
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == infoTabId;
        });
        if (found != tabs.end()) {
            const float infoWidth = preferredInfoWidth;
            const float textWidth = infoWidth - 20.0f;
            const float lineHeight = (lm.detailFont + 1.0f) * 1.25f;
            const std::string titleText = found->name.empty() ? std::string(i18n::tr("tabs.untitled")) : found->name;
            const std::string pathText = std::string(i18n::tr("tabs.path")) + ": " +
                (found->path.empty() ? std::string(i18n::tr("tabs.unsaved_path")) : found->path);
            const std::string rootText = std::string(i18n::tr("tabs.vault")) + ": " +
                displayedRoot(state, *found);
            const float titleH = wrappedHeight(titleText, family, lm.detailFont + 1.0f, textWidth);
            const float pathH = wrappedHeight(pathText, family, lm.detailFont, textWidth);
            const float rootH = wrappedHeight(rootText, family, lm.detailFont, textWidth);
            const float contentH = 10.0f + titleH + pathH + rootH + 10.0f;
            float leftX = x - infoWidth - 8.0f;
            float infoTop = top;
            if (leftX < 8.0f) leftX = std::min(std::max(8.0f, x + width + 8.0f),
                                               std::max(8.0f, screen.width - infoWidth - 8.0f));
            if (infoBelow) { leftX = std::max(8.0f, screen.width - infoWidth - 8.0f); infoTop = top + height + 8.0f; }
            const float maxH = std::max(1.0f, screen.height - infoTop - 8.0f);
            const float boxH = std::min(contentH, maxH);
            const float maxInfoScroll = std::max(0.0f, contentH - boxH);
            infoScroll = std::clamp(infoScroll, 0.0f, maxInfoScroll);
            infoX = leftX; infoY = infoTop; infoW = infoWidth; infoH = boxH;
            const float bridgeLeft = leftX < x ? leftX + infoWidth : x + width;
            const float bridgeRight = leftX < x ? x : leftX;
            if (infoBelow) {
                bridgeX = std::max(leftX, x); bridgeY = top + height;
                bridgeW = std::max(0.0f, std::min(leftX + infoWidth, x + width) - bridgeX); bridgeH = 8.0f;
                sideBridgeX = x + width; sideBridgeY = std::max(top, infoRowTop);
                sideBridgeW = 8.0f; sideBridgeH = std::max(0.0f, top + height + 8.0f - sideBridgeY);
            } else {
                bridgeX = bridgeLeft; bridgeY = infoRowTop;
                bridgeW = std::max(0.0f, bridgeRight - bridgeLeft); bridgeH = infoRowBottom - infoRowTop;
                sideBridgeW = sideBridgeH = 0.0f;
            }
            const auto keepInfoAcrossBridge = [&infoTabId, &hoveredRow, &infoX, &infoY, &infoW, &infoH,
                &infoRowTop, &infoRowBottom, &bridgeX, &bridgeY, &bridgeW, &bridgeH,
                &sideBridgeX, &sideBridgeY, &sideBridgeW, &sideBridgeH,
                panelX = x, panelWidth = width](bool hovered) {
                if (hovered || hoveredRow != 0) return;
                float cx = 0.0f, cy = 0.0f;
                const bool haveCursor = cursorClientPosition(cx, cy);
                const auto inside = [&](float rx, float ry, float rw, float rh) {
                    return haveCursor && rw > 0.0f && rh > 0.0f &&
                        cx >= rx && cx <= rx + rw && cy >= ry && cy <= ry + rh;
                };
                if (!inside(infoX, infoY, infoW, infoH) &&
                    !inside(panelX, infoRowTop, panelWidth, infoRowBottom - infoRowTop) &&
                    !inside(bridgeX, bridgeY, bridgeW, bridgeH) &&
                    !inside(sideBridgeX, sideBridgeY, sideBridgeW, sideBridgeH)) infoTabId = 0;
                app::requestUpdate();
            };
            if (sideBridgeW > 0.0f && sideBridgeH > 0.0f) {
                ui.rect("menubar.tabs.list.info.sideBridge")
                    .position(sideBridgeX, sideBridgeY).size(sideBridgeW, sideBridgeH)
                    .color(transparentColor()).zIndex(2600).onHover(keepInfoAcrossBridge).build();
            }
            if (bridgeW > 0.0f && bridgeH > 0.0f) {
                ui.rect("menubar.tabs.list.info.bridge")
                    .position(bridgeX, bridgeY).size(bridgeW, bridgeH)
                    .color(transparentColor()).zIndex(2600)
                    .onHover(keepInfoAcrossBridge).build();
            }
            ui.stack("menubar.tabs.list.info")
                .position(leftX, infoTop).size(infoWidth, boxH).clip().zIndex(2600)
                .onHover(keepInfoAcrossBridge)
                .onScroll([&infoScroll, maxInfoScroll](const core::ScrollEvent& event) {
                    const float delta = event.y != 0.0 ? -static_cast<float>(event.y) : static_cast<float>(event.x);
                    infoScroll = std::clamp(infoScroll + delta * 36.0f, 0.0f, maxInfoScroll);
                    app::requestUpdate();
                })
                .content([&] {
                    ui.rect("menubar.tabs.list.info.bg").fill().radius(neo::corner(colors, 8.0f))
                        .color(colors.panel).border(1.0f, colors.border)
                        .shadow(components::theme::popupShadow(colors.tokens)).build();
                    const float textX = 10.0f;
                    float cursorY = 10.0f - infoScroll;
                    ui.text("menubar.tabs.list.info.title")
                        .position(textX, cursorY).size(textWidth, titleH)
                        .text(titleText).fontFamily(family).fontSize(lm.detailFont + 1.0f)
                        .color(colors.text).lineHeight(lineHeight)
                        .verticalAlign(eui::VerticalAlign::Top).wrap(true).build();
                    cursorY += titleH;
                    ui.text("menubar.tabs.list.info.path")
                        .position(textX, cursorY).size(textWidth, pathH)
                        .text(pathText).fontFamily(family).fontSize(lm.detailFont)
                        .color(colors.text).lineHeight(lm.detailFont * 1.25f)
                        .verticalAlign(eui::VerticalAlign::Top).wrap(true).build();
                    cursorY += pathH;
                    ui.text("menubar.tabs.list.info.root")
                        .position(textX, cursorY).size(textWidth, rootH)
                        .text(rootText).fontFamily(family).fontSize(lm.detailFont)
                        .color(groupColor(colors, *found)).lineHeight(lm.detailFont * 1.25f)
                        .verticalAlign(eui::VerticalAlign::Top).wrap(true).build();
                }).build();
        }
    }
}

} // namespace tab_bar_detail

inline void tabBarView(eui::Ui& ui, AppState& state, const eui::Screen& screen,
                       float left, float right) {
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    const std::vector<TabInfo> tabs = documentTabs(state);
    const bool interactive = tab_bar_detail::tabSwitchEnabled(state);
    auto& drag = ui.state<tab_bar_detail::DragState>("menubar.tabs.drag");
    if (drag.source == 0) {
        tab_bar_detail::dragInProgress = false;
        tab_bar_detail::escapeCancelRequested = false;
        platform::chrome::armTabDragEscape(false);
    }
    if (drag.source != 0 && (tab_bar_detail::escapeCancelRequested ||
        platform::chrome::tabDragEscapePressed() ||
        !interactive || state.tabListOpen || !platform::chrome::foreground() ||
        drag.activeAtStart != state.tabId || drag.originalOrder != state.tabOrder ||
        std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == drag.source;
        }) == tabs.end())) tab_bar_detail::cancelDrag(ui);
    const auto actions = tab_bar_detail::tabActionsLayout(left, right);
    const float buttonWidth = tab_bar_detail::TabActionsLayout::width;
    const float available = std::max(0.0f, right - left);
    const bool showNew = actions.showNew;
    const bool showList = actions.showList;
    const float listX = actions.listX;
    const float newX = actions.newX;
    const float regionLeft = left;
    const bool flushVaultEdge = state.mode == EditorMode::Vault &&
        std::fabs(left - (std::min(state.vaultWidth, screen.width * 0.38f) + 1.0f)) < 0.5f;
    const float leftBleed = flushVaultEdge ? 0.0f : 8.0f;
    const auto railColor = core::mixColor(colors.toolbar, colors.border, 0.16f);
    const float regionWidth = std::max(0.0f, newX - regionLeft - (showList ? 8.0f : 0.0f));

    if (available > 0.0f) {
        ui.rect("menubar.tabs.region.bg").position(left - leftBleed, 2.0f)
            .size(regionWidth + leftBleed + 8.0f, metrics.menuBarHeight - 4.0f).radius(neo::corner(colors, 10.0f))
            .color(railColor).build();
        if (flushVaultEdge) ui.rect("menubar.tabs.region.squareLeft").position(left, 2.0f)
            .size(std::min(10.0f, regionWidth), metrics.menuBarHeight - 4.0f)
            .color(railColor).build();
    }

    std::uint64_t orderFingerprint = 1469598103934665603ull;
    for (const TabInfo& tab : tabs) {
        orderFingerprint ^= tab.id;
        orderFingerprint *= 1099511628211ull;
    }

    const float contentWidth = static_cast<float>(tabs.size()) * tab_bar_detail::kTabWidth +
                               static_cast<float>(tabs.size() > 0 ? tabs.size() - 1 : 0) *
                                   tab_bar_detail::kTabGap;
    std::size_t activeIndex = 0;
    for (std::size_t index = 0; index < tabs.size(); ++index) {
        if (tabs[index].id == state.tabId) { activeIndex = index; break; }
    }
    // 完整卡放不下时只绘活动页，宽度取 regionWidth（不缩小字体挤出多个不可用标签）。
    const bool activeOnly = !tabs.empty() && regionWidth < tab_bar_detail::kTabWidth;
    if (activeOnly && drag.source != 0) tab_bar_detail::cancelDrag(ui);
    std::vector<TabInfo> displayTabs = tabs;
    if (drag.dragging && !activeOnly) {
        const auto source = std::find_if(displayTabs.begin(), displayTabs.end(), [&](const TabInfo& tab) {
            return tab.id == drag.source;
        });
        if (source != displayTabs.end()) {
            TabInfo moving = *source;
            displayTabs.erase(source);
            displayTabs.insert(displayTabs.begin() + static_cast<std::ptrdiff_t>(
                std::min(drag.targetIndex, displayTabs.size())), std::move(moving));
        }
    }
    std::optional<tab_bar_detail::TabLabelIndex> labels;
    if (!activeOnly && regionWidth > 0.0f && tabs.size() > 1) labels.emplace(tabs);
    const auto* labelIndex = labels ? &*labels : nullptr;
    const float maxScroll = std::max(0.0f, contentWidth - regionWidth);
    float scroll = std::clamp(state.tabScroll, 0.0f, maxScroll);

    std::uint64_t& lastActiveId = ui.state<std::uint64_t>("menubar.tabs.lastActive");
    std::uint64_t& lastOrder = ui.state<std::uint64_t>("menubar.tabs.lastOrder");
    float& lastRegionWidth = ui.state<float>("menubar.tabs.lastRegionWidth");
    const float previousRegionWidth = lastRegionWidth;
    const bool shouldRevealActive = lastActiveId != state.tabId || lastOrder != orderFingerprint ||
                                    std::fabs(lastRegionWidth - regionWidth) > 0.5f;
    lastActiveId = state.tabId;
    lastOrder = orderFingerprint;
    lastRegionWidth = regionWidth;

    // DPI/UI 字号/栏高变化：锚点几何失效，解除关闭链。
    float& lastBarHeight = ui.state<float>("menubar.tabs.lastBarHeight");
    float& lastUiFontSize = ui.state<float>("menubar.tabs.lastUiFontSize");
    float& lastScale = ui.state<float>("menubar.tabs.lastScale");
    float& lastScreenHeight = ui.state<float>("menubar.tabs.lastScreenHeight");
    const float scale = effectiveWindowScale();
    if (state.closeAnchor.enabled &&
        (std::fabs(lastBarHeight - metrics.menuBarHeight) > 0.01f ||
         std::fabs(lastUiFontSize - state.uiFontSize) > 0.01f ||
         std::fabs(lastScale - scale) > 0.01f ||
         std::fabs(lastScreenHeight - screen.height) > 0.5f)) {
        tab_bar_detail::clearCloseAnchor(state);
    }
    lastBarHeight = metrics.menuBarHeight;
    lastUiFontSize = state.uiFontSize;
    lastScale = scale;
    lastScreenHeight = screen.height;

    if (state.closeAnchor.enabled &&
        (state.tabListOpen || documentModalOpen(state) || state.settingsOpen ||
         std::fabs(previousRegionWidth - regionWidth) > 0.5f)) {
        tab_bar_detail::clearCloseAnchor(state);
    }

    float closeDrawOffset = 0.0f;
    if (state.closeAnchor.enabled) {
        tabpresentation::layout::CloseAnchorInput input;
        input.enabled = state.closeAnchor.enabled;
        input.anchorX = state.closeAnchor.anchorX;
        input.closedTabId = state.closeAnchor.closedTabId;
        input.expectedNextId = state.closeAnchor.expectedNextId;
        input.beforeTabOrder = state.closeAnchor.beforeTabOrder;
        const tabpresentation::layout::CloseAnchorPlan plan =
            tabpresentation::layout::planCloseAnchor(input, state.tabOrder, state.tabId, scroll);
        if (!plan.valid || !std::isfinite(plan.drawOffset) ||
            state.closeAnchor.anchorX < 0.0f || state.closeAnchor.anchorX > regionWidth ||
            state.closeAnchor.anchorY < 0.0f || state.closeAnchor.anchorY > metrics.menuBarHeight) {
            tab_bar_detail::clearCloseAnchor(state);
        } else {
            // 指针真正离开顶栏标签区域才解除（用真实光标位置与区域矩形判断，不靠元素 onHover）。
            float cursorX = 0.0f, cursorY = 0.0f;
            const bool haveCursor = cursorClientPosition(cursorX, cursorY);
            const bool outside = haveCursor &&
                (cursorX < regionLeft || cursorX > regionLeft + regionWidth ||
                 cursorY < 0.0f || cursorY > metrics.menuBarHeight);
            if (outside) {
                tab_bar_detail::clearCloseAnchor(state);
            } else {
                if (state.closeAnchorPendingApply) {
                    state.closeAnchor.drawOffset = plan.drawOffset;
                    state.closeAnchorPendingApply = false;
                }
                closeDrawOffset = state.closeAnchor.drawOffset;
            }
        }
    }

    if (activeOnly) {
        scroll = 0.0f;
    } else if ((shouldRevealActive || state.closeAnchorRevealPending) && !tabs.empty() &&
               !state.closeAnchor.enabled && !drag.dragging) {
        const float activeLeft = static_cast<float>(activeIndex) * tab_bar_detail::kTabPitch;
        const float activeRight = activeLeft + tab_bar_detail::kTabWidth;
        if (activeLeft < scroll) scroll = activeLeft;
        else if (activeRight > scroll + regionWidth) scroll = activeRight - regionWidth;
        scroll = std::clamp(scroll, 0.0f, maxScroll);
    }
    state.tabScroll = scroll;
    if (!state.closeAnchor.enabled) state.closeAnchorRevealPending = false;

    if (drag.dragging) {
        const float margin = std::min(28.0f, regionWidth * 0.25f);
        const int edge = drag.pointerX < regionLeft + margin ? -1 :
            drag.pointerX > regionLeft + regionWidth - margin ? 1 : 0;
        const bool canScroll = (edge < 0 && state.tabScroll > 0.0f) ||
                               (edge > 0 && state.tabScroll < maxScroll);
        ui.rect("menubar.tabs.drag.timer").size(0.0f, 0.0f)
            .onTimer(canScroll ? 1.0f / 60.0f : 0.1f,
                [&ui, &state, regionLeft, edge, maxScroll] {
                    auto& current = ui.state<tab_bar_detail::DragState>("menubar.tabs.drag");
                    if (!current.dragging) return;
                    if (tab_bar_detail::escapeCancelRequested ||
                        platform::chrome::tabDragEscapePressed() ||
                        !platform::chrome::foreground() ||
                        current.activeAtStart != state.tabId || current.originalOrder != state.tabOrder ||
                        !tab_bar_detail::tabSwitchEnabled(state)) {
                        tab_bar_detail::cancelDrag(ui);
                        return;
                    }
                    if (edge != 0) {
                        state.tabScroll = std::clamp(state.tabScroll + edge * 8.0f, 0.0f, maxScroll);
                        current.targetIndex = tab_bar_detail::dragTargetIndex(
                            current.pointerX, current.grabOffset, regionLeft,
                            state.tabScroll, state.tabOrder.size());
                    }
                }).build();
    }

    if (regionWidth > 0.0f && !tabs.empty()) {
        ui.stack("menubar.tabs.clip")
            .position(regionLeft - leftBleed, 0.0f)
            .size(regionWidth + leftBleed + 8.0f, metrics.menuBarHeight + 1.0f).clip()
            .onScroll([&state, maxScroll](const core::ScrollEvent& event) {
                if (!tab_bar_detail::tabSwitchEnabled(state)) return;
                // 用户滚轮横移：解除关闭链，回到普通布局。
                if (state.closeAnchor.enabled) tab_bar_detail::clearCloseAnchor(state);
                const float delta = event.x != 0.0 ? static_cast<float>(event.x) : -static_cast<float>(event.y);
                state.tabScroll = std::clamp(state.tabScroll + delta * 36.0f, 0.0f, maxScroll);
                app::requestUpdate();
            })
            .content([&] {
                ui.stack("menubar.tabs.cards").position(leftBleed, 0.0f)
                    .size(regionWidth, metrics.menuBarHeight).content([&] {
                    if (activeOnly) {
                        platform::chrome::exclude(regionLeft, 4, regionWidth, metrics.menuBarHeight-4);
                        tab_bar_detail::tabCard(ui, state, colors, metrics, tabs[activeIndex], tabs,
                                                0.0f, regionWidth, true, interactive, false, labelIndex,
                                                regionLeft, regionWidth, maxScroll, false, false, flushVaultEdge);
                        return;
                    }
                    // Paint the active face last so its shoulders cover adjacent cards.
                    for (int pass = 0; pass < 2; ++pass)
                    for (std::size_t index = 0; index < displayTabs.size(); ++index) {
                        const TabInfo& tab = displayTabs[index];
                        if ((tab.id == state.tabId) != (pass == 1)) continue;
                        const float x = static_cast<float>(index) * tab_bar_detail::kTabPitch - scroll +
                                        closeDrawOffset;
                        const float visibleLeft = std::max(0.0f, x);
                        const float visibleRight = std::min(regionWidth, x+tab_bar_detail::kTabWidth);
                        platform::chrome::exclude(regionLeft+visibleLeft, 4, visibleRight-visibleLeft, metrics.menuBarHeight-4);
                        tab_bar_detail::tabCard(ui, state, colors, metrics, tab, tabs, x,
                                                tab_bar_detail::kTabWidth, tab.id == state.tabId, interactive,
                                                true, labelIndex, regionLeft, regionWidth, maxScroll,
                                                tabs.size() > 1, drag.dragging && tab.id == drag.source,
                                                flushVaultEdge && std::fabs(x) < 0.5f);
                    }
                }).build();
            }).build();
    }

    // Two fixed-width overlays only when content overflows. No blur, animation,
    // offscreen mask or per-tab opacity updates; all pointer events pass through.
    if (!activeOnly && regionWidth > 0.0f) {
        const float fadeWidth = std::min(20.0f, regionWidth * 0.25f);
        const auto clearRail = rgba(railColor.r, railColor.g, railColor.b, 0.0f);
        if (scroll - closeDrawOffset > 0.5f)
            ui.rect("menubar.tabs.fade.left").position(regionLeft - leftBleed, 4.0f)
                .size(fadeWidth + leftBleed, metrics.menuBarHeight - 4.0f)
                .gradient(railColor, clearRail, core::GradientDirection::Horizontal)
                .hitTestMode(core::dsl::HitTestMode::None).build();
        if (contentWidth - scroll + closeDrawOffset > regionWidth + 0.5f)
            ui.rect("menubar.tabs.fade.right").position(regionLeft + regionWidth - fadeWidth, 4.0f)
                .size(fadeWidth + 8.0f, metrics.menuBarHeight - 4.0f)
                .gradient(clearRail, railColor, core::GradientDirection::Horizontal)
                .hitTestMode(core::dsl::HitTestMode::None).build();
    }

    if (drag.dragging && regionWidth > 0.0f) {
        const auto source = std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == drag.source;
        });
        if (source != tabs.end()) {
            const float ghostX = std::clamp(drag.pointerX - drag.grabOffset,
                regionLeft, std::max(regionLeft, regionLeft + regionWidth - tab_bar_detail::kTabWidth));
            ui.stack("menubar.tabs.dragGhost")
                .position(ghostX, 3.0f).size(tab_bar_detail::kTabWidth, metrics.menuBarHeight - 3.0f)
                .zIndex(2100).hitTestMode(core::dsl::HitTestMode::None)
                .content([&] {
                    ui.rect("menubar.tabs.dragGhost.bg").fill().radius(neo::corner(colors, 8.0f))
                        .color(colors.editor).border(1.0f, colors.accent)
                        .shadow(components::theme::popupShadow(colors.tokens)).build();
                    ui.text("menubar.tabs.dragGhost.label")
                        .position(12.0f, 0.0f).size(tab_bar_detail::kTabWidth - 42.0f, metrics.menuBarHeight - 3.0f)
                        .text(tab_bar_detail::elideMeasured(tab_bar_detail::tabLabel(*source, tabs, labelIndex),
                            uiFontFamily(state), metrics.menuFontSize, tab_bar_detail::kTabWidth - 42.0f))
                        .fontFamily(uiFontFamily(state)).fontSize(metrics.menuFontSize)
                        .color(colors.text).verticalAlign(eui::VerticalAlign::Center).wrap(false).build();
                }).build();
        }
    }

    // Rounded action group, separated from both the tab shoulders and the
    // native window controls; keep the existing draggable space to its right.
    if (actions.showPin) {
        // Keep the whole visible group in the client area. Caption seams cause
        // WM_MOUSELEAVE to clear the shared pointer and interrupt Reveal light.
        // Individual DSL hit boxes still own their actions and hover tooltips.
        platform::chrome::exclude(newX, 3.0f, right - newX, metrics.menuBarHeight - 6.0f);
        ui.rect("menubar.tabs.actions.bg").position(newX, 3.0f)
            .size(right - newX, metrics.menuBarHeight - 6.0f).radius(neo::corner(colors, 10.0f))
            .color(core::mixColor(colors.toolbar, colors.editor, 0.72f))
            .border(1.0f, core::mixColor(colors.toolbar, colors.border, 0.65f))
            .hitTestMode(core::dsl::HitTestMode::None).build();
    }
    if (showNew) {
        platform::chrome::exclude(newX, 4, buttonWidth-2, metrics.menuBarHeight-8);
        ui.rect("menubar.tabs.new.hit")
            .position(newX + 1.0f, 4.0f).size(buttonWidth - 2.0f, metrics.menuBarHeight - 8.0f)
            .radius(neo::corner(colors, 7.0f)).states(transparentColor(), colors.rowHover, colors.pressed)
            .preserveFocusOnPress().disabled(!interactive)
            .onClick([&state, &ui, interactive] {
                if (interactive) {
                    state.tabListOpen = false;
                    tab_bar_detail::clearCloseAnchor(state);
                    newDocument(state);
                    ui.requestFocus(editorInputId(state.tabId) + ".hit");
                    app::requestUpdate();
                }
            }).build();
        iconView(ui, "menubar.tabs.new.icon", UiIcon::Plus,
                 newX + 5.0f, (metrics.menuBarHeight - 18.0f) * 0.5f, 18.0f, colors.textMuted);
    }
    if (showList) {
        platform::chrome::exclude(listX, 4, buttonWidth-2, metrics.menuBarHeight-8);
        ui.rect("menubar.tabs.list.hit")
            .position(listX + 1.0f, 4.0f).size(buttonWidth - 2.0f, metrics.menuBarHeight - 8.0f)
            .radius(neo::corner(colors, 7.0f)).states(state.tabListOpen ? colors.rowHover : transparentColor(),
                                  colors.rowHover, colors.pressed)
            .preserveFocusOnPress().disabled(!interactive)
            .onClick([&state, &ui, interactive] {
                if (!interactive) return;
                tab_bar_detail::clearCloseAnchor(state);
                state.tabListOpen = !state.tabListOpen;
                state.tabListReveal = state.tabId;
                ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                if (state.tabListOpen) {
                    ui.state<std::uint64_t>("menubar.tabs.listLastReveal") = 0;
                    ui.requestFocus("menubar.tabs.list.keys");
                }
                else ui.requestFocus(editorInputId(state.tabId) + ".hit");
                app::requestUpdate();
            }).build();
        iconView(ui, "menubar.tabs.list.icon", UiIcon::ChevronDown,
                 listX + 5.0f, (metrics.menuBarHeight - 16.0f) * 0.5f, 16.0f, colors.textMuted);
    }

    if (actions.showPin) tab_bar_detail::pinButtonView(ui, colors, actions.pinX, metrics.menuBarHeight, interactive);

    if (state.tabListOpen) {
        ui.rect("menubar.tabs.list.keys").size(0.0f, 0.0f).focusable()
            .onKeyEvent([&state, &ui](const eui::KeyEvent& event) {
                if (!event.isDown() || event.key != eui::InputKey::Escape) return false;
                state.tabListOpen = false;
                ui.state<std::uint64_t>("menubar.tabs.hovered") = 0;
                ui.state<std::uint64_t>("menubar.tabs.list.infoTab") = 0;
                ui.requestFocus(editorInputId(state.tabId) + ".hit");
                app::requestUpdate();
                return true;
            }).build();
    }
    tab_bar_detail::tabList(ui, state, screen, colors, metrics, tabs, right + 8.0f, interactive);

    std::uint64_t& hoveredId = ui.state<std::uint64_t>("menubar.tabs.hovered");
    if (hoveredId != 0 && !state.tabListOpen && !drag.dragging) {
        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const TabInfo& tab) {
            return tab.id == hoveredId;
        });
        if (found != tabs.end()) {
            const float fontSize = std::max(9.0f, metrics.menuFontSize - 2.0f);
            const std::string pathText = std::string(i18n::tr("tabs.path")) + ": " +
                (found->path.empty() ? std::string(i18n::tr("tabs.unsaved_path")) : found->path);
            const std::string rootText = std::string(i18n::tr("tabs.vault")) + ": " +
                tab_bar_detail::displayedRoot(state, *found);
            const float maxWidth = std::max(0.0f, std::min(480.0f, screen.width - 16.0f));
            const float measuredWidth = std::max(
                core::TextPrimitive::measureTextWidth(pathText, uiFontFamily(state), fontSize),
                core::TextPrimitive::measureTextWidth(rootText, uiFontFamily(state), fontSize));
            const float tooltipWidth = std::min(maxWidth, std::max(std::min(120.0f, maxWidth), measuredWidth + 20.0f));
            const float x = std::clamp(left + 12.0f, 8.0f, std::max(8.0f, screen.width - tooltipWidth - 8.0f));
            const float y = metrics.menuBarHeight + 2.0f;
            const float textWidth = std::max(0.0f, tooltipWidth - 20.0f);
            const float lineHeight = fontSize * 1.25f;
            const float pathHeight = tab_bar_detail::wrappedHeight(pathText, uiFontFamily(state), fontSize, textWidth);
            const float rootHeight = tab_bar_detail::wrappedHeight(rootText, uiFontFamily(state), fontSize, textWidth);
            const float tooltipHeight = 12.0f + pathHeight + rootHeight;
            ui.stack("menubar.tabs.tooltip")
                .excludeFromBackdropSnapshot()
                .position(x, y).size(tooltipWidth, tooltipHeight).zIndex(2600)
                .content([&] {
                    ui.rect("menubar.tabs.tooltip.bg").fill().radius(neo::corner(colors, 7.0f))
                        .color(colors.panel).border(1.0f, colors.border)
                        .shadow(components::theme::popupShadow(colors.tokens)).build();
                    ui.text("menubar.tabs.tooltip.path")
                        .position(10.0f, 6.0f).size(textWidth, pathHeight)
                        .text(pathText)
                        .fontFamily(uiFontFamily(state)).fontSize(fontSize)
                        .color(colors.text).lineHeight(lineHeight).verticalAlign(eui::VerticalAlign::Center).wrap(true).build();
                    ui.text("menubar.tabs.tooltip.root")
                        .position(10.0f, 6.0f + pathHeight).size(textWidth, rootHeight)
                        .text(rootText)
                        .fontFamily(uiFontFamily(state)).fontSize(fontSize)
                        .color(tab_bar_detail::groupColor(colors, *found))
                        .lineHeight(lineHeight).verticalAlign(eui::VerticalAlign::Center).wrap(true).build();
                }).build();
        } else {
            hoveredId = 0;
        }
    }
}

} // namespace neo
