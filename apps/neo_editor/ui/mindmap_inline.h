#pragma once

// Read-only inline mindmap; source bytes, undo history and save state stay in InputModel.
#include "model/mindmap_cache.h"
#include "model/mindmap_plan.h"
#include "state/app_state.h"
#include "state/mindmap_preview_state.h"
#include "ui/metrics.h"
#include "ui/mindmap_preview.h"

namespace neo::mindmap_inline {

using InputModel = components::input_detail::InputModel;
using LineDecoration = components::input_detail::LineDecoration;
using LineDecorationSnapshot = components::input_detail::LineDecorationSnapshot;
using TextLine = InputModel::TextLine;
using Rect = core::Rect;

inline constexpr float kHeaderHeight = 36.0f;
inline constexpr float kCanvasHeight = 320.0f;
inline constexpr float kInlinePanelHeight = kHeaderHeight + kCanvasHeight;

// Open the exact code block whose fence line owns the button. Do not fake a cursor move just to
// reuse openMindmapModel(cursor): that would overwrite user selection/caret state. The state helper
// should be factored into openMindmapModelAt(blockBeg), with the modal entry point remaining a
// thin wrapper that passes the current cursor-selected target.
inline mindmap::RejectReason openInlineMindmap(eui::Ui& ui, AppState& state, int blockBeg) {
    auto& input = ui.state<InputModel::InputState>(editorInputId(state));
    if (!input.compositionText.empty()) {
        return mindmap::RejectReason::PlanNotReady; // Keep the active IME composition untouched.
    }
    if (state.sourceView() || !state.markdownCapable()) {
        return mindmap::RejectReason::NotMarkdown;
    }

    const lp::PlanCache& lpCache = lp::planCache();
    if (!lpCache.valid || lpCache.text != state.doc.text || input.text != state.doc.text) {
        return mindmap::RejectReason::PlanNotReady;
    }
    const auto target = mindmap::selectTarget(state.doc.text, lpCache.plan, blockBeg);
    if (!target || target.blockBeg != blockBeg) {
        return target.rejectReason == mindmap::RejectReason::None
            ? mindmap::RejectReason::InvalidFence : target.rejectReason;
    }
    if (!state.derivedMindmap) state.derivedMindmap = std::make_unique<mindmap::Cache>();
    mindmap::Cache& cache = *state.derivedMindmap;

    const bool warm = cache.key.codeLang == "mermaid" && cache.key.tabId == state.tabId && cache.key.path == state.path &&
        cache.key.revision == state.revision && cache.key.textRevision == input.textRevision &&
        cache.key.blockBeg == static_cast<std::size_t>(target.blockBeg) &&
        cache.key.blockEnd == static_cast<std::size_t>(target.blockEnd) &&
        mindmap_detail::mindmapBlockAnchorCurrent(cache, state, lpCache);
    if (!warm) {
        const std::size_t previousBytes = cache.bytes;
        const std::string_view source(state.doc.text.data() + target.blockBeg,
            static_cast<std::size_t>(target.blockEnd - target.blockBeg));
        auto parsed = mindmap::parseBlock(source,
            static_cast<std::size_t>(target.blockBeg));
        if (!parsed) return parsed.rejectReason;

        cache.release();
        cache.plan = std::move(parsed.plan);
        cache.source.assign(source);
        cache.key = mindmap::CacheKey{};
        cache.key.tabId = state.tabId;
        cache.key.path = state.path;
        cache.key.revision = state.revision;
        cache.key.textRevision = input.textRevision;
        cache.key.blockBeg = cache.plan.sourceBeg;
        cache.key.blockEnd = cache.plan.sourceEnd;
        cache.key.codeLang = "mermaid";
        cache.valid = false;
        cache.bytes = 0;
        ++cache.generation;
        state.derivedCacheBytes -= std::min<std::uint64_t>(state.derivedCacheBytes, previousBytes);
    }

    mindmap::PreviewState& preview = state.mindmapPreview;
    preview.close();
    preview.tabId = state.tabId;
    preview.path = state.path;
    preview.revision = state.revision;
    preview.textRevision = input.textRevision;
    preview.open = true;
    preview.inlineMode = true;
    preview.viewport = ImageViewport{};
    preview.viewport.focusPending = true; // Focus the viewer after its first retained-tree build.
    input.followCaret = false;
    // A menu open may originate below the fence. Reveal its host without moving the caret.
    for (std::size_t i = 0; i < input.cachedLines.size(); ++i) {
        if (input.cachedLines[i].start == blockBeg && input.cachedGeometry.count() > static_cast<int>(i)) {
            input.verticalScroll = std::min(input.verticalScroll, input.cachedGeometry.top(static_cast<int>(i))); break;
        }
    }
    ++input.decorationRevision;
    app::requestUpdate();
    return mindmap::RejectReason::None;
}

// Called before InputBuilder::build(). It prepares the same page-owned parsed plan, label sizes,
// layout and cache budget as the modal preview. Keep the fixed 320 logical-pixel canvas for v1;
// panelWidth, effective DPI, fonts, theme and theme-file version remain in the existing style key.
inline bool prepareInlineMindmap(eui::Ui& ui, AppState& state, float panelWidth) {
    mindmap::PreviewState& preview = state.mindmapPreview;
    if (!preview.open || !preview.inlineMode || !state.derivedMindmap) return false;

    auto& input = ui.state<InputModel::InputState>(editorInputId(state));
    const lp::PlanCache& lpCache = lp::planCache();
    if (!input.compositionText.empty()) {
        preview.close();
        preview.inlineMode = false;
        ++input.decorationRevision;
        app::requestUpdate();
        return false; // Never hide or blur an active composition.
    }
    if (!mindmapModelCurrent(preview, *state.derivedMindmap, state, input, lpCache)) {
        preview.close(); // Stale text/tab/block always falls back to editable source.
        preview.inlineMode = false;
        ++input.decorationRevision;
        app::requestUpdate();
        return false;
    }

    // The first frame must draw the viewer before focus is transferred, or the source-focused
    // guard below would immediately close it. Once focusPending is cleared, normal editor focus
    // plus cursor/selection entering the hidden source block closes it before layout is built.
    if (!preview.viewport.focusPending && ui.isFocused(editorInputId(state) + ".hit")) {
        const int beg = static_cast<int>(state.derivedMindmap->plan.sourceBeg);
        const int end = static_cast<int>(state.derivedMindmap->plan.sourceEnd);
        const int selBeg = std::min(input.selectionStart, input.selectionEnd);
        const int selEnd = std::max(input.selectionStart, input.selectionEnd);
        const bool caretInside = input.cursor >= beg && input.cursor < end;
        const bool selectionCrosses = selBeg != selEnd && selBeg < end && selEnd > beg;
        if (caretInside || selectionCrosses) {
            preview.close();
            preview.inlineMode = false;
            ++input.decorationRevision;
            app::requestUpdate();
            return false;
        }
    }

    mindmap::Cache& cache = *state.derivedMindmap;
    const UiMetrics metrics = uiMetrics(state);
    MindmapStyleKey style{
        editorFontFamily(state), codeFontFamily(state), state.themeFile,
        metrics.editorFontSize, effectiveWindowScale(), panelWidth,
        static_cast<int>(state.theme), themeRevision()
    };
    if (mindmapCacheCurrent(cache, preview, style)) return true;

    std::vector<mindmap::Size> sizes =
        measureMindmapLabels(cache.plan, editorFontFamily(state), metrics.editorFontSize);
    mindmap::Layout layout = mindmap::buildLayout(cache.plan, sizes);
    const std::size_t oldBytes = cache.bytes;
    if (!storeMindmapCache(cache, preview, style, std::move(sizes), std::move(layout))) {
        state.derivedCacheBytes -= std::min<std::uint64_t>(state.derivedCacheBytes, oldBytes);
        preview.close();
        preview.inlineMode = false;
        ++input.decorationRevision;
        showToast(state, i18n::tr("mindmap.title"), i18n::tr("mindmap.too_large"));
        app::requestUpdate();
        return false; // Limits or budget rejection leaves the actual document source untouched.
    }
    state.derivedCacheBytes = state.derivedCacheBytes -
        std::min<std::uint64_t>(state.derivedCacheBytes, oldBytes) + cache.bytes;
    // Style/width changes reset geometry but must not steal focus from other
    // editable source. Only the original open transfers focus to the viewer.
    const bool focusPending = preview.viewport.focusPending;
    preview.viewport = ImageViewport{};
    preview.viewport.focusPending = focusPending;
    return true;
}

// Decorator composition sketch: preserve the cached LP snapshot, then overlay only this one
// active block. lineHeight on the host source row expands that row; the remaining source rows
// become zero-height view-hidden rows. The planned hiddenByView bit must stay separate from
// hiddenByFold so onRevealHiddenLine can never turn a view toggle into UnfoldAt.
inline LineDecorationSnapshot inlineMindmapDecorations(
    const AppState& state, LineDecorationSnapshot base, float panelHeight) {
    if (!state.mindmapPreview.open || !state.mindmapPreview.inlineMode ||
        !state.derivedMindmap || !base) return base;

    const mindmap::Cache& cache = *state.derivedMindmap;
    const LpPlan& plan = lp::planCache().plan;
    const int first = plan.lineIndexFor(static_cast<int>(cache.plan.sourceBeg));
    const int last = plan.lineIndexFor(static_cast<int>(cache.plan.sourceEnd) - 1);
    if (first < 0 || last < first || last >= static_cast<int>(base->size())) return base;
    if ((*base)[static_cast<std::size_t>(first)].hiddenByFold) return base;

    const LpLine& hostSource = plan.lines[static_cast<std::size_t>(first)];
    std::vector<int> changedRows;
    std::vector<LineDecoration> patches;
    changedRows.reserve(static_cast<std::size_t>(last - first + 1));
    patches.reserve(static_cast<std::size_t>(last - first + 1));
    LineDecoration host = (*base)[static_cast<std::size_t>(first)];
    host.lineHeight = panelHeight;
    host.holes.push_back({hostSource.srcBeg, hostSource.srcEnd});
    host.hidden = false;
    host.hiddenByFold = false;
    host.hiddenByView = false;
    changedRows.push_back(first);
    patches.push_back(std::move(host));
    for (int index = first + 1; index <= last; ++index) {
        LineDecoration row = (*base)[static_cast<std::size_t>(index)];
        if (row.hiddenByFold) continue; // Preserve actual Markdown folding semantics.
        row.hidden = true;
        row.hiddenByFold = false;
        row.hiddenByView = true;
        changedRows.push_back(index);
        patches.push_back(std::move(row));
    }
    return base->replacing(changedRows, std::move(patches));
}

// InputBuilder lineOverlay callback sketch. The planned callback supplies input-local coordinates
// for one visible lineStart: x=inset, y=textY+geometry.top-scroll, width=textWidth, height=row height.
// The button and graph therefore stay inside the existing clipped text viewport and follow scroll,
// wrap, resize and DPI geometry without guessing screen coordinates in editor_view.
inline void closeInlineMindmap(eui::Ui& ui, AppState& state) {
    auto& preview = state.mindmapPreview;
    preview.close();
    preview.inlineMode = false;
    auto& input = ui.state<InputModel::InputState>(editorInputId(state));
    ++input.decorationRevision;
    ui.requestFocus(editorInputId(state) + ".hit");
    app::requestUpdate();
}

inline void drawInlineMindmapLine(
    eui::Ui& ui, AppState& state, const TextLine& line, const Rect& inputLocal) {
    mindmap::PreviewState& preview = state.mindmapPreview;
    if (!line.lineStart || !state.markdownCapable() || state.sourceView()) return;

    const LpPlan& sourcePlan = lp::planCache().plan;
    const int sourceRow = sourcePlan.lineIndexFor(line.start);
    const bool closedMermaid = sourceRow >= 0 && sourceRow < static_cast<int>(sourcePlan.lines.size()) && [&] {
        const LpLine& open = sourcePlan.lines[static_cast<std::size_t>(sourceRow)];
        if (!open.codeFence || open.codeLang != "mermaid" || open.codeBlockBeg != line.start || open.codeBlockEnd <= line.start)
            return false;
        const int closeRow = sourcePlan.lineIndexFor(open.codeBlockEnd - 1);
        return closeRow > sourceRow && closeRow < static_cast<int>(sourcePlan.lines.size()) &&
            sourcePlan.lines[static_cast<std::size_t>(closeRow)].codeFence &&
            sourcePlan.lines[static_cast<std::size_t>(closeRow)].codeBlockBeg == line.start;
    }();
    const bool activeBlock = preview.open && preview.inlineMode && state.derivedMindmap &&
        line.start == static_cast<int>(state.derivedMindmap->plan.sourceBeg);
    if (!activeBlock) {
        // LP owner and closing-fence metadata exclude unsupported and unterminated fences.
        if (!closedMermaid) return;
        const float buttonWidth = 76.0f;
        const float buttonHeight = std::max(14.0f, std::min(26.0f, inputLocal.height));
        const float x = inputLocal.x + std::max(0.0f, inputLocal.width - buttonWidth - 8.0f);
        const float y = inputLocal.y;
        const std::string toggleId = "mindmapInline.toggle." + std::to_string(line.start);
        ui.stack(toggleId).position(x, y).size(buttonWidth, buttonHeight)
            .content([&] {
                ui.rect(toggleId + ".bg").size(buttonWidth, buttonHeight)
                    .radius(4.0f).states(editorColors(state.theme).toolbar, editorColors(state.theme).rowHover, editorColors(state.theme).pressed)
                    .cursor(eui::CursorShape::Hand)
                    .onPress([&ui, &state, blockBeg = line.start](const core::PointerEvent&, const core::Rect&) {
                        const auto reason = openInlineMindmap(ui, state, blockBeg);
                        if (reason != mindmap::RejectReason::None) {
                            showToast(state, i18n::tr("mindmap.title"),
                                      i18n::tr(mindmapRejectMessage(reason)));
                        }
                    }).preserveFocusOnPress().build();
                ui.text(toggleId + ".label").position(4.0f, 0.0f)
                    .size(buttonWidth - 8.0f, buttonHeight).text(i18n::tr("mindmap.render"))
                    .fontFamily(uiFontFamily(state)).fontSize(std::min(uiMetrics(state).menuFontSize, buttonHeight / 1.3f))
                    .color(editorColors(state.theme).text)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center).build();
            }).build();
        return;
    }

    if (!state.derivedMindmap || !state.derivedMindmap->valid) return;
    mindmap::Cache& cache = *state.derivedMindmap;
    if (line.start != static_cast<int>(cache.plan.sourceBeg)) return;

    const EditorColors& colors = editorColors(state.theme);
    const UiMetrics metrics = uiMetrics(state);
    const mindmap::Layout& layout = cache.layout;
    const float width = inputLocal.width;
    const float height = inputLocal.height;
    const float canvasHeight = std::max(1.0f, height - kHeaderHeight);
    auto& view = preview.viewport;
    const float fitWidth = std::max(layout.bounds.width, 480.0f);
    const float fitHeight = std::max(layout.bounds.height, 320.0f);
    view.constrain(fitWidth, fitHeight, width, canvasHeight);
    const float viewScale = std::max(0.001f,
        std::min(std::max(1.0f, width * 0.92f) / fitWidth,
                 std::max(1.0f, canvasHeight * 0.92f) / fitHeight) * view.zoom);
    const float worldCenterX = layout.bounds.x + layout.bounds.width * 0.5f;
    const float worldCenterY = layout.bounds.y + layout.bounds.height * 0.5f;
    const float panelCenterX = width * 0.5f + view.panX;
    const float panelCenterY = canvasHeight * 0.5f + view.panY;
    const auto toX = [&](float x) { return panelCenterX + (x - worldCenterX) * viewScale; };
    const auto toY = [&](float y) { return panelCenterY + (y - worldCenterY) * viewScale; };
    const float margin = 48.0f;
    const mindmap::Rect worldViewport{
        worldCenterX - (width * 0.5f + view.panX + margin) / viewScale,
        worldCenterY - (canvasHeight * 0.5f + view.panY + margin) / viewScale,
        (width + 2.0f * margin) / viewScale,
        (canvasHeight + 2.0f * margin) / viewScale};
    const mindmap::Visibility visible = mindmap::queryVisible(layout, worldViewport);
    const std::string hitId = editorInputId(state) + ".mindmap.inline.hit";

    ui.stack("mindmapInline.canvas").position(inputLocal.x, inputLocal.y + kHeaderHeight)
        .size(width, canvasHeight).clip().content([&] {
    ui.rect("mindmapInline.canvas.bg").size(width, canvasHeight).color(colors.editor).build();
    for (const int edgeIndex : visible.edges) {
        const mindmap::Edge& edge = layout.edges[static_cast<std::size_t>(edgeIndex)];
        if (edge.pointCount < 4) continue;
        for (int segment = 0; segment < 3; ++segment) {
            const mindmap::Point& a = edge.points[segment];
            const mindmap::Point& b = edge.points[segment + 1];
            const float x0 = toX(a.x);
            const float y0 = toY(a.y);
            const float x1 = toX(b.x);
            const float y1 = toY(b.y);
            ui.rect("mindmapInline.edge." + std::to_string(edge.child) + "." + std::to_string(segment))
                .position(std::min(x0, x1), std::min(y0, y1))
                .size(std::max(1.0f, std::fabs(x1 - x0)), std::max(1.0f, std::fabs(y1 - y0)))
                .color(colors.border).build();
        }
    }
    for (const int nodeIndex : visible.nodes) {
        const std::size_t index = static_cast<std::size_t>(nodeIndex);
        const mindmap::LayoutNode& node = layout.nodes[index];
        const float x = toX(node.bounds.x);
        const float y = toY(node.bounds.y);
        const float nodeWidth = std::max(1.0f, node.bounds.width * viewScale);
        const float nodeHeight = std::max(1.0f, node.bounds.height * viewScale);
        const float radius = cache.plan.nodes[index].shape == mindmap::Shape::Circle
            ? std::min(nodeWidth, nodeHeight) * 0.5f
            : std::min(6.0f * viewScale, std::min(nodeWidth, nodeHeight) * 0.5f);
        const std::string id = "mindmapInline.node." + std::to_string(nodeIndex);
        ui.rect(id + ".border").position(x - 1.0f, y - 1.0f)
            .size(nodeWidth + 2.0f, nodeHeight + 2.0f).radius(radius + 1.0f)
            .color(colors.border).build();
        ui.rect(id + ".fill").position(x, y).size(nodeWidth, nodeHeight)
            .radius(radius).color(colors.toolbar).build();
        if (!visible.textOmitted) {
            ui.text(id + ".label")
                .position(x + nodeWidth * 0.5f - node.bounds.width * 0.5f,
                          y + nodeHeight * 0.5f - node.bounds.height * 0.5f)
                .size(std::max(1.0f, node.bounds.width), std::max(1.0f, node.bounds.height))
                .text(cache.plan.nodes[index].label).fontFamily(editorFontFamily(state))
                .fontSize(metrics.editorFontSize).color(colors.text)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).transformOrigin(0.5f, 0.5f)
                .scale(viewScale, viewScale).build();
        }
    }

    });
    ui.rect("mindmapInline.header.bg").position(inputLocal.x, inputLocal.y)
        .size(width, kHeaderHeight).color(colors.toolbar).build();
    ui.text("mindmapInline.header.title").position(inputLocal.x + 12.0f, inputLocal.y)
        .size(std::max(1.0f, width - 200.0f), kHeaderHeight)
        .text(i18n::tr("mindmap.title")).fontFamily(uiFontFamily(state))
        .fontSize(metrics.menuFontSize).color(colors.text)
        .verticalAlign(eui::VerticalAlign::Center).build();
    ui.text("mindmapInline.header.source").position(inputLocal.x + width - 164.0f, inputLocal.y + 4.0f)
        .size(76.0f, 28.0f).text(i18n::tr("mindmap.source"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.menuFontSize).color(colors.text)
        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
    ui.text("mindmapInline.header.fit").position(inputLocal.x + width - 80.0f, inputLocal.y + 4.0f)
        .size(68.0f, 28.0f).text(i18n::tr("mindmap.fit"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.menuFontSize).color(colors.text)
        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();

    // One read-only hit surface routes header actions, zoom, pan and Escape. Composition must
    // be empty before entering this focused surface; it never writes a document byte.
    ui.rect(hitId).position(inputLocal.x, inputLocal.y).size(width, height)
        .color(core::Color{0, 0, 0, 0}).focusable()
        .acceptedButtons(core::PointerButton::Left | core::PointerButton::Middle)
        .dragThreshold(0.0f)
        .onMove([&view, width, canvasHeight](const core::PointerEvent& event, const core::Rect& bounds) {
            view.pointerX = static_cast<float>(event.x) - bounds.x;
            view.pointerY = static_cast<float>(event.y) - bounds.y - kHeaderHeight;
            view.pointerKnown = view.pointerX >= 0.0f && view.pointerX <= width &&
                view.pointerY >= 0.0f && view.pointerY <= canvasHeight;
            return false;
        })
        .onPress([&ui, &state, width, canvasHeight, fitWidth, fitHeight](
                     const core::PointerEvent& event, const core::Rect& bounds) {
            if (event.button != core::PointerButton::Left) return;
            const float x = static_cast<float>(event.x) - bounds.x;
            const float y = static_cast<float>(event.y) - bounds.y;
            auto& activeView = state.mindmapPreview.viewport;
            if (y <= kHeaderHeight) {
                if (x >= width - 176.0f && x < width - 88.0f) {
                    closeInlineMindmap(ui, state);
                } else if (x >= width - 88.0f) {
                    activeView.zoom = 1.0f;
                    activeView.panX = activeView.panY = 0.0f;
                    activeView.constrain(fitWidth, fitHeight, width, canvasHeight);
                    app::requestUpdate();
                }
            }
        })
        .onDrag([&view, fitWidth, fitHeight, width, canvasHeight](const core::dsl::DragEvent& event) {
            if (event.button != core::PointerButton::Middle) return;
            view.panX += static_cast<float>(event.deltaX);
            view.panY += static_cast<float>(event.deltaY);
            view.constrain(fitWidth, fitHeight, width, canvasHeight);
        })
        .onScroll([&view, &state, fitWidth, fitHeight, width, canvasHeight](const core::ScrollEvent& event) {
            if (!state.mindmapPreview.open) return;
            // Wide/deep trees may fit at far below 1/16. Permit their cached
            // labels to reach at least their normal world-space font size.
            const float fit = std::max(0.001f, view.fit(fitWidth, fitHeight, width, canvasHeight));
            view.wheel(static_cast<float>(event.y), fitWidth, fitHeight, width, canvasHeight,
                       std::max(16.0f, 1.0f / fit));
        })
        .onKeyEvent([&ui, &state](const core::KeyEvent& event) {
            if (event.isDown() && event.key == core::InputKey::Escape) closeInlineMindmap(ui, state);
            return !event.modifiers.shortcut();
        })
        .onTextInput([](const core::TextInputEvent& event) {
            if (event.composing || event.compositionChanged)
                core::window::cancelImeComposition(core::window::mainWindowHandle());
        })
        .cursor(eui::CursorShape::Arrow).build();

    if (visible.textOmitted) {
        ui.text("mindmapInline.textOmitted").position(inputLocal.x + 8.0f, inputLocal.y + height - 24.0f)
            .size(std::max(1.0f, width - 16.0f), 20.0f).text(i18n::tr("mindmap.text_omitted"))
            .fontFamily(uiFontFamily(state)).fontSize(metrics.menuFontSize).color(colors.text)
            .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
    }

    if (preview.viewport.focusPending) {
        ui.requestFocus(hitId);
        preview.viewport.focusPending = false;
    }
}

} // namespace neo::mindmap_inline
