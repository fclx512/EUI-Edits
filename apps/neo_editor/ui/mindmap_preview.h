#pragma once

// 树与布局仅驻留于页缓存；浮层不接正文编辑，也不建立自己的 InputModel。

#include "state/app_actions.h"
#include "state/app_state.h"
#include "state/mindmap_preview_state.h"
#include "ui/metrics.h"

#include "core/render/text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {

inline void closeMindmapPreview(AppState& state) {
    state.mindmapPreviewPending = false;
    state.mindmapPreview.close();  // 页自有的 derivedMindmap 原样保留（暖开、可淘汰）
    state.findEditorFocusPending = true;
    app::requestUpdate();
}

// RejectReason → toast 文案键（拒绝必须可观测）。
inline const char* mindmapRejectMessage(mindmap::RejectReason reason) {
    using R = mindmap::RejectReason;
    switch (reason) {
        case R::None: return "";
        case R::PlanNotReady: return "mindmap.not_ready";
        case R::NotMarkdown:
        case R::NoBlock:
        case R::UnclosedFence:
        case R::InvalidFence:
            return "mindmap.no_target";
        case R::SourceLimit:
        case R::LabelLimit:
        case R::DepthLimit:
        case R::NodeLimit:
            return "mindmap.too_large";
        default:
            return "mindmap.unsupported";
    }
}

// View 菜单排队的 compose 侧消费者（事件阶段拿不到输入组件光标）。拒绝 → toast +
// 保持关闭，绝不排队等下一帧。
inline void openMindmapPreview(eui::Ui& ui, AppState& state) {
    using InputModel = components::input_detail::InputModel;
    InputModel::InputState& input = ui.state<InputModel::InputState>(editorInputId(state));
    if (!state.derivedMindmap) state.derivedMindmap = std::make_unique<mindmap::Cache>();
    const std::size_t previousBytes = state.derivedMindmap->bytes;
    const mindmap::RejectReason reason = openMindmapModel(
        state.mindmapPreview, *state.derivedMindmap, state, input, lp::planCache());
    state.derivedCacheBytes = state.derivedCacheBytes - std::min<std::uint64_t>(
        state.derivedCacheBytes, previousBytes) + state.derivedMindmap->bytes;
    if (reason != mindmap::RejectReason::None) {
        showToast(state, i18n::tr("mindmap.title"), i18n::tr(mindmapRejectMessage(reason)));
    }
    app::requestUpdate();
}

// 标签测字：TextPrimitive 逻辑口径（layoutPixelScale 全局由框架管理），行高用
// measureTextSize 的真实高度（中文/emoji 不按系数估）。
inline std::vector<mindmap::Size> measureMindmapLabels(const mindmap::Plan& plan,
                                                       const char* fontFamily, float fontSize) {
    std::vector<mindmap::Size> sizes(plan.nodes.size());
    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        core::TextStyle style;
        style.text = plan.nodes[index].label;
        style.fontFamily = fontFamily;
        style.fontSize = fontSize;
        const core::Vec2 measured = core::TextPrimitive::measureTextSize(style);
        sizes[index] = mindmap::Size{std::max(1.0f, measured.x), std::max(1.0f, measured.y)};
    }
    return sizes;
}

inline void mindmapPreviewOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    mindmap::PreviewState& preview = state.mindmapPreview;
    if (!preview.open) return;

    using InputModel = components::input_detail::InputModel;
    InputModel::InputState& input = ui.state<InputModel::InputState>(editorInputId(state));
    if (!state.derivedMindmap) state.derivedMindmap = std::make_unique<mindmap::Cache>();
    mindmap::Cache& cache = *state.derivedMindmap;
    // 陈旧/外部回写：不渲染旧树、不排队等帧（按需渲染没有定时器）——直接关闭。
    if (!mindmapModelCurrent(preview, cache, state, input, lp::planCache())) {
        closeMindmapPreview(state);
        return;
    }

    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);

    // 面板（92% × 88%，居中）。宽度进缓存键：变化即重测重建。
    const float panelWidth = std::max(120.0f, screen.width * 0.92f);
    const float panelHeight = std::max(90.0f, screen.height * 0.88f);
    const float panelX = (screen.width - panelWidth) * 0.5f;
    const float panelY = (screen.height - panelHeight) * 0.5f;
    MindmapStyleKey style;
    style.editorFontFamily = editorFontFamily(state);
    style.codeFontFamily = codeFontFamily(state);
    style.themeFile = state.themeFile;
    style.fontSize = metrics.editorFontSize;
    style.effectiveScale = effectiveWindowScale();
    style.panelWidth = panelWidth;
    style.theme = static_cast<int>(state.theme);
    style.themeVersion = themeRevision();

    if (!mindmapCacheCurrent(cache, preview, style)) {
        std::vector<mindmap::Size> sizes =
            measureMindmapLabels(cache.plan, editorFontFamily(state), metrics.editorFontSize);
        mindmap::Layout layout = mindmap::buildLayout(cache.plan, sizes);
        const std::size_t previousBytes = cache.bytes;
        if (!storeMindmapCache(cache, preview, style, std::move(sizes), std::move(layout))) {
            state.derivedCacheBytes -= std::min<std::uint64_t>(state.derivedCacheBytes, previousBytes);
            showToast(state, i18n::tr("mindmap.title"), i18n::tr("mindmap.too_large"));
            closeMindmapPreview(state);
            return;
        }
        // 活动页计账：换掉旧代字节（captureDerivedCaches 离场时会整体重算）。
        state.derivedCacheBytes = state.derivedCacheBytes - std::min<std::uint64_t>(
            state.derivedCacheBytes, previousBytes) + cache.bytes;
        preview.viewport = ImageViewport{};  // 新几何从 fit 视角重新开始
    }

    const mindmap::Layout& layout = cache.layout;
    auto& view = preview.viewport;
    // 锚点缩放/平移/constrain 用同一组托底尺寸，口径一致。
    const float fitWidth = std::max(layout.bounds.width, 480.0f);
    const float fitHeight = std::max(layout.bounds.height, 320.0f);
    view.constrain(fitWidth, fitHeight, panelWidth, panelHeight);
    const float viewScale =
        std::max(0.001f, std::min(std::max(1.0f, panelWidth * 0.92f) / fitWidth,
                                  std::max(1.0f, panelHeight * 0.92f) / fitHeight) *
                             view.zoom);

    const float worldCenterX = layout.bounds.x + layout.bounds.width * 0.5f;
    const float worldCenterY = layout.bounds.y + layout.bounds.height * 0.5f;
    // 面板**局部**坐标（stack 子元素相对父框架定位）。
    const float panelCenterX = panelWidth * 0.5f + view.panX;
    const float panelCenterY = panelHeight * 0.5f + view.panY;
    const auto toPanelX = [&](float worldX) {
        return panelCenterX + (worldX - worldCenterX) * viewScale;
    };
    const auto toPanelY = [&](float worldY) {
        return panelCenterY + (worldY - worldCenterY) * viewScale;
    };

    // 可见性查询（世界坐标视口 + 边缘缓冲）：只构建可见节点/连线。
    const float margin = 48.0f;
    const mindmap::Rect worldViewport{
        worldCenterX - (panelWidth * 0.5f + view.panX + margin) / viewScale,
        worldCenterY - (panelHeight * 0.5f + view.panY + margin) / viewScale,
        (panelWidth + 2.0f * margin) / viewScale, (panelHeight + 2.0f * margin) / viewScale};
    const mindmap::Visibility visible = mindmap::queryVisible(layout, worldViewport);

    ui.stack("mindmapPreview")
        .size(screen.width, screen.height)
        .zIndex(2800)
        .clip()
        .content([&] {
            ui.rect("mindmapPreview.dim")
                .size(screen.width, screen.height)
                .zIndex(2800)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.85f})
                .build();

            ui.stack("mindmapPreview.panel")
                .position(panelX, panelY)
                .size(panelWidth, panelHeight)
                .zIndex(2801)
                .clip()
                .content([&] {
                    ui.rect("mindmapPreview.panel.bg")
                        .size(panelWidth, panelHeight)
                        .radius(8.0f)
                        .color(colors.editor)
                        .build();

                    // 连线（正交折线，恒 3 段），压在节点下面。
                    const float thickness = std::max(1.0f, 1.4f * viewScale);
                    for (const int edgeIndex : visible.edges) {
                        const mindmap::Edge& edge = layout.edges[static_cast<std::size_t>(edgeIndex)];
                        if (edge.pointCount < 4) continue;
                        const std::string id =
                            "mindmapPreview.edge." + std::to_string(edge.child);
                        for (int segment = 0; segment < 3; ++segment) {
                            const mindmap::Point& a = edge.points[segment];
                            const mindmap::Point& b = edge.points[segment + 1];
                            const float x0 = toPanelX(a.x), y0 = toPanelY(a.y);
                            const float x1 = toPanelX(b.x), y1 = toPanelY(b.y);
                            ui.rect(id + "." + std::to_string(segment))
                                .position(std::min(x0, x1), std::min(y0, y1))
                                .size(std::max(thickness, std::fabs(x1 - x0)),
                                      std::max(thickness, std::fabs(y1 - y0)))
                                .zIndex(1)
                                .color(colors.border)
                                .build();
                        }
                    }

                    // 节点：只画可见的；文字用缓存字号 + transform 缩放（不改字号、
                    // 不重测），概览降级时只画框。
                    const char* labelFont = editorFontFamily(state);
                    for (const int nodeIndex : visible.nodes) {
                        const std::size_t index = static_cast<std::size_t>(nodeIndex);
                        const mindmap::LayoutNode& node = layout.nodes[index];
                        const std::string id = "mindmapPreview.node." + std::to_string(nodeIndex);
                        const float x = toPanelX(node.bounds.x);
                        const float y = toPanelY(node.bounds.y);
                        const float width = std::max(1.0f, node.bounds.width * viewScale);
                        const float height = std::max(1.0f, node.bounds.height * viewScale);
                        const float radius =
                            cache.plan.nodes[index].shape == mindmap::Shape::Circle
                                ? std::min(width, height) * 0.5f
                                : std::min(6.0f * viewScale, std::min(width, height) * 0.5f);
                        ui.rect(id + ".border")
                            .position(x - 1.0f, y - 1.0f)
                            .size(width + 2.0f, height + 2.0f)
                            .radius(radius + 1.0f)
                            .zIndex(2)
                            .color(colors.border)
                            .build();
                        ui.rect(id + ".fill")
                            .position(x, y)
                            .size(width, height)
                            .radius(std::max(0.0f, radius))
                            .zIndex(2)
                            .color(colors.toolbar)
                            .build();
                        if (!visible.textOmitted) {
                            // 排字盒用**未缩放**的世界尺寸，位置按"缩放后中心 - 未缩放
                            // 半径"校准，再 transformOrigin(0.5) + scale：文字按缓存
                            // 字号渲染、几何缩放不改字号不重测，也与节点框严格同比。
                            const float centerX = x + width * 0.5f;
                            const float centerY = y + height * 0.5f;
                            ui.text(id + ".label")
                                .position(centerX - node.bounds.width * 0.5f,
                                          centerY - node.bounds.height * 0.5f)
                                .size(std::max(1.0f, node.bounds.width),
                                      std::max(1.0f, node.bounds.height))
                                .text(cache.plan.nodes[index].label)
                                .fontFamily(labelFont)
                                .fontSize(metrics.editorFontSize)
                                .color(colors.text)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .transformOrigin(0.5f, 0.5f)
                                .scale(viewScale, viewScale)
                                .zIndex(3)
                                .build();
                        }
                    }

                    // 面板头部（视觉；命中统一走全屏 hit 的 onPress 路由，因为全屏
                    // hit z1302 压住面板内任何 hit）。
                    const float headerHeight = 36.0f;
                    ui.rect("mindmapPreview.header.bg")
                        .position(0.0f, 0.0f)
                        .size(panelWidth, headerHeight)
                        .zIndex(4)
                        .color(colors.toolbar)
                        .build();
                    ui.text("mindmapPreview.header.title")
                        .position(12.0f, 0.0f)
                        .size(panelWidth - 200.0f, headerHeight)
                        .text(i18n::tr("mindmap.title"))
                        .fontFamily(uiFontFamily(state))
                        .fontSize(metrics.menuFontSize)
                        .color(colors.text)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .zIndex(4)
                        .build();
                    const auto headerLabel = [&](const char* idSuffix, float x, float width,
                                                 const char* label) {
                        ui.text(std::string("mindmapPreview.header.") + idSuffix)
                            .position(x, 4.0f)
                            .size(width, headerHeight - 8.0f)
                            .text(label)
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.menuFontSize)
                            .color(colors.text)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .zIndex(4)
                            .build();
                    };
                    headerLabel("fit", panelWidth - 176.0f, 88.0f, i18n::tr("mindmap.fit"));
                    headerLabel("close", panelWidth - 80.0f, 56.0f, i18n::tr("mindmap.close"));

                    if (visible.textOmitted) {
                        ui.rect("mindmapPreview.hint.bg")
                            .position(0.0f, panelHeight - 32.0f)
                            .size(panelWidth, 32.0f)
                            .zIndex(4)
                            .color(colors.toolbar)
                            .build();
                        ui.text("mindmapPreview.hint")
                            .position(0.0f, panelHeight - 32.0f)
                            .size(panelWidth, 32.0f)
                            .text(i18n::tr("mindmap.text_omitted"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.menuFontSize)
                            .color(colors.text)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .zIndex(4)
                            .build();
                    }
                })
                .build();

            // 全屏命中面：左键只在**面板外空白**关闭；中键平移、滚轮锚点缩放、Esc
            // 关闭；IME 合成即取消，候选窗抢不走 Esc。
            ui.rect("mindmapPreview.hit")
                .size(screen.width, screen.height)
                .zIndex(2802)
                .color(core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                .acceptedButtons(core::PointerButton::Left | core::PointerButton::Middle)
                .dragThreshold(0.0f)
                .onMove([&state, panelX, panelY, panelWidth, panelHeight](const core::PointerEvent& event,
                                                 const core::Rect&) -> bool {
                    const float x = static_cast<float>(event.x) - panelX;
                    const float y = static_cast<float>(event.y) - panelY;
                    state.mindmapPreview.viewport.pointerX = x;
                    state.mindmapPreview.viewport.pointerY = y;
                    state.mindmapPreview.viewport.pointerKnown =
                        x >= 0.0f && x <= panelWidth && y >= 36.0f && y <= panelHeight;
                    return false;
                })
                .onPress([&state, panelX, panelY, panelWidth, panelHeight](
                             const core::PointerEvent& event, const core::Rect&) {
                    if (event.button != core::PointerButton::Left) return;
                    const float x = static_cast<float>(event.x);
                    const float y = static_cast<float>(event.y);
                    // 头部按钮（屏幕坐标；全屏 hit 压住面板内 hit，命中在这里路由）。
                    const float buttonTop = panelY + 4.0f;
                    const float buttonBottom = buttonTop + 28.0f;
                    if (y >= buttonTop && y <= buttonBottom) {
                        if (x >= panelX + panelWidth - 176.0f && x <= panelX + panelWidth - 88.0f) {
                            state.mindmapPreview.viewport.zoom = 1.0f;
                            state.mindmapPreview.viewport.panX = 0.0f;
                            state.mindmapPreview.viewport.panY = 0.0f;
                            app::requestUpdate();
                            return;
                        }
                        if (x >= panelX + panelWidth - 80.0f && x <= panelX + panelWidth - 24.0f) {
                            closeMindmapPreview(state);
                            return;
                        }
                    }
                    // 左键只在**面板外空白**关闭；面板内其余区域不关闭（v1 无节点点击）。
                    const bool insidePanel = x >= panelX && x <= panelX + panelWidth &&
                                             y >= panelY && y <= panelY + panelHeight;
                    if (!insidePanel) closeMindmapPreview(state);
                })
                .onDrag([&state, fitWidth, fitHeight, panelWidth, panelHeight](
                            const core::dsl::DragEvent& event) {
                    if (event.button != core::PointerButton::Middle) return;
                    auto& viewport = state.mindmapPreview.viewport;
                    viewport.panX += static_cast<float>(event.deltaX);
                    viewport.panY += static_cast<float>(event.deltaY);
                    viewport.constrain(fitWidth, fitHeight, panelWidth, panelHeight);
                })
                .onScroll([&state, fitWidth, fitHeight, panelWidth, panelHeight](
                              const core::ScrollEvent& event) {
                    state.mindmapPreview.viewport.wheel(static_cast<float>(event.y), fitWidth,
                                                        fitHeight, panelWidth, panelHeight);
                })
                .onKeyEvent([&state](const core::KeyEvent& event) {
                    if (event.isDown() && event.key == core::InputKey::Escape) {
                        closeMindmapPreview(state);
                    }
                    return true;
                })
                .onTextInput([](const core::TextInputEvent& event) {
                    if (event.composing || event.compositionChanged) {
                        core::window::cancelImeComposition(core::window::mainWindowHandle());
                    }
                })
                .onContextMenu([](const core::PointerEvent&, const core::Rect&) {})
                .cursor(eui::CursorShape::Arrow)
                .build();
        })
        .build();

    if (view.focusPending) {
        view.focusPending = false;
        ui.requestFocus("mindmapPreview.hit");
    }
}

} // namespace neo
