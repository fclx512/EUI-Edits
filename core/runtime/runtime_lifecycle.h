#pragma once

namespace core::dsl {

inline bool Runtime::initialize() {
    return true;
}

inline bool Runtime::initialize(core::window::Handle window) {
    inputWindow_ = window;
    core::window::installInputCallbacks(window);
    return true;
}

template <typename ComposeFn>
inline void Runtime::compose(const std::string& pageId, float logicalWidth, float logicalHeight, ComposeFn&& composeFn) {
    const std::vector<runtime::ElementSnapshot> previousStructure = elementStructure_;
    const Screen screen{logicalWidth, logicalHeight};
    ui_.begin(pageId);
    ui_.setFocusedId(focusedId_);
    composeFn(ui_, screen);
    ui_.end();
    ui_.layout(screen);
    std::string requestedFocus;
    if (ui_.consumeFocusRequest(requestedFocus)) {
        const Element* target = ui_.find(requestedFocus);
        if (target != nullptr && target->focusable && !isElementInDisabledTree(requestedFocus)) {
            setFocusedId(requestedFocus);
        }
    }
    elementStructure_ = collectElementStructure();
    syncScrollStateBindings(true);
    for (const std::string& scope : ui_.consumeReleasedStateScopes()) {
        const std::string childPrefix = scope + ".";
        if (focusedId_ == scope || focusedId_.rfind(childPrefix, 0) == 0) {
            focusedId_.clear();
            ui_.setFocusedId(focusedId_);
        }
    }
    if (!focusedId_.empty() && isElementInDisabledTree(focusedId_)) {
        setFocusedId({});
    }

    if (elementStructure_ != previousStructure) {
        paintRequested_ = true;
        fullPaintRequested_ = true;
        pruneInstancesRequested_ = true;
    }

    if (logicalWidth_ != logicalWidth || logicalHeight_ != logicalHeight) {
        paintRequested_ = true;
        fullPaintRequested_ = true;
    }
    fullTreeUpdateRequested_ = true;
    logicalWidth_ = logicalWidth;
    logicalHeight_ = logicalHeight;
}

inline bool Runtime::update(core::window::Handle window, float deltaSeconds, float pointerScale, float dpiScale, bool inputEnabled) {
    inputWindow_ = window;
    ++updateFrameToken_;
    if (updateFrameToken_ == 0) {
        ++updateFrameToken_;
    }
    if (!inputEnabled) {
        cancelTextComposition();
        cancelInput(window);
    }
    std::vector<PointerEvent> pointerEvents = consumePointerEvents(window, pointerScale);
    std::vector<KeyEvent> keyEvents = consumeKeyEvents(window);
    TextInputEvent textInputEvent = consumeTextInput(window);
    ScrollEvent scrollEvent = consumeScrollInput(window);
    if (!inputEnabled) {
        for (PointerEvent& event : pointerEvents) {
            event.x = -1000000.0;
            event.y = -1000000.0;
            event.deltaX = 0.0;
            event.deltaY = 0.0;
            event.modifiers = {};
        }
        keyEvents.clear();
        textInputEvent = {};
        scrollEvent = {};
    }
    animating_ = false;
    nextTimerWakeSeconds_ = std::numeric_limits<float>::infinity();
    composeRequested_ = false;
    // 悬停光标只在**有指针事件**的帧重置：按需渲染下，定时器（光标闪烁）/后台
    // 唤醒产生的帧没有指针事件，若照旧每帧重置，applyCursor 会在鼠标静止时把
    // I-beam 打回默认箭头（实测：编辑器停住不动一个闪烁周期就变回箭头）。
    // 没有指针事件时维持上一帧的悬停光标——悬停目标只有在指针移动或布局变化时
    // 才会变，而布局变化的帧必然伴随重绘与后续指针事件。
    if (!pointerEvents.empty()) {
        hoverCursor_ = CursorShape::Arrow;
    }
    if (pruneInstancesRequested_) {
        instances_.markInstancesUnseen();
    }
    instances_.markTimersUnseen();
    if (ImagePrimitive::consumeRemoteImageReady()) {
        fullPaintRequested_ = true;
        paintRequested_ = true;
    }

    syncScrollStateBindings(false);
    if (scrollEvent.active()) {
        updateScroll(scrollEvent, hitTestScrollable(pointerEvents.back(), dpiScale));
        hoverTargetCacheValid_ = false;
    }
    updateScrollMotion(deltaSeconds);

    for (std::size_t index = 0; index < pointerEvents.size(); ++index) {
        const PointerEvent& event = pointerEvents[index];
        if (event.isPress(PointerButton::Left) || event.isPress(PointerButton::Right)) {
            // Drain the old owner before a pointer can move its insertion point
            // or choose another control. Candidate-window clicks are native and
            // do not enter this client pointer queue.
            if (textInputEvent.hasInput()) {
                updateTextInput(textInputEvent);
                textInputEvent = {};
            }
            cancelTextComposition();
        }
        if (event.isPress(PointerButton::Left)) {
            setFocusedId(hitTestFocusable(event, dpiScale));
        }
        const std::string hoverTargetId = resolveHoverTarget(event, dpiScale, inputEnabled);
        const float eventDeltaSeconds = index + 1 == pointerEvents.size() ? deltaSeconds : 0.0f;
        updateElementTree(event, eventDeltaSeconds, dpiScale, hoverTargetId);
    }
    flushPendingScrollDirtyRects();
    updateDependentVisualDirtyRegions(dpiScale);

    if (!keyEvents.empty()) {
        updateKeyInput(keyEvents);
    }
    if (textInputEvent.hasInput()) {
        updateTextInput(textInputEvent);
    }
    instances_.releaseUnseenTimers();
    updateImeCursorRect(window, dpiScale);
    applyCursor(window);

    promoteBackdropBlurDirtyRegions(dpiScale);
    if (pruneInstancesRequested_) {
        instances_.releaseUnseenInstances();
        pruneInstancesRequested_ = false;
    }
    fullTreeUpdateRequested_ = false;
    previousFrameAnimating_ = animating_;

    const bool result = paintRequested_;
    paintRequested_ = false;
    return result;
}

inline bool Runtime::isAnimating() const {
    return animating_;
}

inline float Runtime::nextTimerWakeSeconds() const {
    return nextTimerWakeSeconds_;
}

inline bool Runtime::composeRequested() const {
    return composeRequested_;
}

inline bool Runtime::paintRequested() const {
    return paintRequested_;
}

inline void Runtime::requestFullPaint() {
    fullPaintRequested_ = true;
    paintRequested_ = true;
}

inline void Runtime::render(int windowWidth, int windowHeight, float dpiScale, const Color& clearColor) {
    core::render::RenderBackend* renderBackend = core::render::activeRenderBackend();
    if (renderBackend == nullptr) {
        return;
    }

    core::render::beginRenderFrameStats(windowWidth, windowHeight);
    ImagePrimitive::beginRenderFrame();
    core::render::RenderFrameStats& stats = core::render::currentRenderFrameStats();

    const bool hasRenderableContent = !ui_.roots().empty();
    const auto releasePrunedRetainedLayers = [&] {
        instances_.releaseUnseenRetainedLayers();
    };
    if (!hasRenderableContent) {
        ++stats.clearCalls;
        renderBackend->clear(clearColor);
        dirtyRects_.clear();
        fullPaintRequested_ = false;
        releasePrunedRetainedLayers();
        core::render::publishRenderFrameStats();
        return;
    }

    if (!renderBackend->ensureRenderCache(windowWidth, windowHeight)) {
        ++stats.clearCalls;
        renderBackend->clear(clearColor);
        ++stats.renderDirectPasses;
        RuntimeRenderer(ui_, instances_).renderDirect(
            *renderBackend, windowWidth, windowHeight, dpiScale);
        dirtyRects_.clear();
        fullPaintRequested_ = false;
        releasePrunedRetainedLayers();
        core::render::publishRenderFrameStats();
        return;
    }
    stats.usedRenderCache = true;
    if (renderBackend->renderCacheWasRecreated()) {
        fullPaintRequested_ = true;
        stats.renderCacheRecreated = true;
    }

    if (!fullPaintRequested_ && dirtyRects_.empty()) {
        renderBackend->blitRenderCache(windowWidth, windowHeight, core::render::RenderCacheBlitMode::Existing);
        releasePrunedRetainedLayers();
        core::render::publishRenderFrameStats();
        return;
    }

    stats.fullPaint = fullPaintRequested_;
    const std::vector<Rect> dirtyRects = fullPaintRequested_
        ? std::vector<Rect>{}
        : core::dsl::resolveDirtyRects(dirtyRects_, windowWidth, windowHeight, dpiScale);
    if (!fullPaintRequested_ && dirtyRects.empty()) {
        dirtyRects_.clear();
        renderBackend->blitRenderCache(windowWidth, windowHeight, core::render::RenderCacheBlitMode::Existing);
        releasePrunedRetainedLayers();
        core::render::publishRenderFrameStats();
        return;
    }
    stats.dirtyRectCount = static_cast<int>(dirtyRects.size());
    for (const Rect& dirty : dirtyRects) {
        const float width = std::max(0.0f, dirty.width);
        const float height = std::max(0.0f, dirty.height);
        stats.dirtyPixels += static_cast<std::uint64_t>(width * height);
    }

    renderBackend->beginRenderCacheFrame(windowWidth, windowHeight, dirtyRects);

    if (fullPaintRequested_) {
        renderBackend->setScissor(false, {}, windowHeight);
        ++stats.clearCalls;
        renderBackend->clear(clearColor);
        ++stats.renderDirectPasses;
        RuntimeRenderer(ui_, instances_).renderDirect(
            *renderBackend, windowWidth, windowHeight, dpiScale);
    } else {
        for (const Rect& dirty : dirtyRects) {
            renderBackend->setScissor(true, dirty, windowHeight);
            ++stats.clearCalls;
            renderBackend->clear(clearColor);
            ++stats.renderDirectPasses;
            RuntimeRenderer(ui_, instances_).renderDirect(
                *renderBackend, windowWidth, windowHeight, dpiScale, &dirty);
        }
        renderBackend->setScissor(false, {}, windowHeight);
    }

    renderBackend->endRenderCacheFrame();
    renderBackend->blitRenderCache(windowWidth,
                                   windowHeight,
                                   fullPaintRequested_ ? core::render::RenderCacheBlitMode::Full
                                                       : core::render::RenderCacheBlitMode::Dirty,
                                   dirtyRects);
    const bool retainedLayerWarmupNeeded =
        stats.retainedLayerMisses > 0 && stats.retainedLayerRebuilds == 0;
    dirtyRects_.clear();
    fullPaintRequested_ = retainedLayerWarmupNeeded;
    paintRequested_ = retainedLayerWarmupNeeded;
    releasePrunedRetainedLayers();
    core::render::publishRenderFrameStats();
}

inline void Runtime::render(int windowWidth, int windowHeight, float dpiScale) {
    core::render::RenderBackend* renderBackend = core::render::activeRenderBackend();
    if (renderBackend == nullptr) {
        return;
    }

    ImagePrimitive::beginRenderFrame();

    RuntimeRenderer(ui_, instances_).renderDirect(
        *renderBackend, windowWidth, windowHeight, dpiScale);
    instances_.releaseUnseenRetainedLayers();
}

inline void Runtime::shutdown(bool releaseCachedImageTextures) {
    cancelTextComposition();
    inputWindow_ = nullptr;
    releaseGraphicsResources(releaseCachedImageTextures);
    instances_.clear();
    elementStructure_.clear();
    hoverTargetCacheValid_ = false;
    // 元素与回调也可能持有外部 GPU 资源，不能留到设备销毁后的 Runtime 析构。
    ui_.begin();
    ui_.end();
    ui_.clearState();
    keyEventHandler_ = {};
}

inline void Runtime::releaseGraphicsResources(bool releaseCachedImageTextures) {
    instances_.releaseGraphicsResources(releaseCachedImageTextures);
    destroyCursors();
    fullPaintRequested_ = true;
    paintRequested_ = true;
}

inline void Runtime::applyCursor(core::window::Handle window) {
    if (!arrowCursor_) {
        arrowCursor_ = core::window::createStandardCursor(core::window::CursorType::Arrow);
    }

    // Hand 只表示"这是可点元素"，实际画什么由应用的 interactiveCursor_ 决定；
    // IBeam（文本输入）是元素的明确要求，不做映射。
    const CursorShape shape = hoverCursor_ == CursorShape::Hand ? interactiveCursor_ : hoverCursor_;
    core::window::CursorHandle target = arrowCursor_;
    if (shape == CursorShape::Hand) {
        if (!handCursor_) {
            handCursor_ = core::window::createStandardCursor(core::window::CursorType::Hand);
        }
        target = handCursor_ != nullptr ? handCursor_ : arrowCursor_;
    } else if (shape == CursorShape::IBeam) {
        if (!ibeamCursor_) {
            ibeamCursor_ = core::window::createStandardCursor(core::window::CursorType::IBeam);
        }
        target = ibeamCursor_ != nullptr ? ibeamCursor_ : arrowCursor_;
    }
    if (target != currentCursor_) {
        core::window::setCursor(window, target);
        currentCursor_ = target;
    }
}

inline void Runtime::destroyCursors() {
    if (arrowCursor_) {
        core::window::destroyCursor(arrowCursor_);
        arrowCursor_ = nullptr;
    }
    if (handCursor_) {
        core::window::destroyCursor(handCursor_);
        handCursor_ = nullptr;
    }
    if (ibeamCursor_) {
        core::window::destroyCursor(ibeamCursor_);
        ibeamCursor_ = nullptr;
    }
    currentCursor_ = nullptr;
}

} // namespace core::dsl
