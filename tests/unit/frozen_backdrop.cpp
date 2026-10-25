#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

bool near(float a, float b, float epsilon = 0.01f) {
    return std::fabs(a - b) <= epsilon;
}

enum class SurfaceTag { Backdrop, Menu, TopBar, Tooltip, Other };

SurfaceTag tagFor(const core::Rect& rect) {
    if (near(rect.x, 50.0f) && near(rect.y, 50.0f)) return SurfaceTag::Backdrop;
    if (near(rect.x, 100.0f) || near(rect.x, 112.0f)) return SurfaceTag::Menu;
    if (near(rect.x, 10.0f) && near(rect.y, 2.0f)) return SurfaceTag::TopBar;
    if (near(rect.x, 210.0f) || near(rect.x, 224.0f)) return SurfaceTag::Tooltip;
    return SurfaceTag::Other;
}

struct FrozenFrameRequest {
    std::uint64_t session = 0;
    std::uint64_t sceneRevision = 0;
    int width = 0;
    int height = 0;
    float dpiScale = 1.0f;
};

class FrozenBackdropRecorder final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return true; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}

    bool beginFrozenBackdropFrame(std::uint64_t session, std::uint64_t sceneRevision,
                                  int width, int height, float dpiScale) override {
        requests.push_back({session, sceneRevision, width, height, dpiScale});
        if (session == 0) {
            lastSession = 0;
            lastRevision = 0;
            lastWidth = lastHeight = 0;
            frozenPoisoned = false;
            settling = false;
            settleFrames = 0;
            captureActive = false;
            return false;
        }
        if (session != lastSession) {
            lastSession = session;
            lastRevision = sceneRevision;
            lastWidth = width;
            lastHeight = height;
            lastDpi = dpiScale;
            frozenPoisoned = false;
            settling = true;
            settleFrames = 1;
            captureActive = false;
            core::render::currentRenderFrameStats().frozenCaptureDeferred = true;
            return false;
        }
        if (settling && !frozenPoisoned) {
            if (sceneRevision == lastRevision && width == lastWidth && height == lastHeight &&
                near(dpiScale, lastDpi)) {
                settling = false;
                captureActive = true;
                return true;
            }
            lastRevision = sceneRevision;
            lastWidth = width;
            lastHeight = height;
            lastDpi = dpiScale;
            if (++settleFrames > 3) {
                settling = false;
                invalidateSession();
                return false;
            }
            captureActive = false;
            core::render::currentRenderFrameStats().frozenCaptureDeferred = true;
            return false;
        }
        if (sceneRevision != lastRevision || width != lastWidth || height != lastHeight ||
            !near(dpiScale, lastDpi)) {
            invalidateSession();
        }
        captureActive = false;
        return false;
    }
    void captureFrozenBackdropScene() override {
        ++captures;
        capturedSessions.push_back(lastSession);
        captureActive = false;
    }
    bool cachedStripNeedsValidation(std::uint64_t) const override { return true; }
    bool beginCachedStripFrame(std::uint64_t session, std::uint64_t revision,
                               int width, int height, float dpi) override {
        stripRequests.push_back({session, revision, width, height, dpi});
        return requestStripCapture && session != 0;
    }
    void captureCachedStrip(const core::FrozenBackdrop& material) override {
        capturedStrip = material;
        stripCopyRects = normalRects;
    }
    void prepareFrozenBackdrop(const core::FrozenBackdrop& material) override {
        preparedMaterials.push_back(material);
    }
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (command.shadowPass) return;
        const float dpi = requests.empty() ? 1.0f : requests.back().dpiScale;
        const SurfaceTag tag = tagFor({command.rect.x / dpi, command.rect.y / dpi,
                                      command.rect.width / dpi, command.rect.height / dpi});
        if (captureActive) cleanRects.push_back(tag);
        else normalRects.push_back(tag);
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {
        if (captureActive) ++cleanTextDraws;
        else ++normalTextDraws;
    }

    void resetFrameObservations() {
        cleanRects.clear();
        normalRects.clear();
        cleanTextDraws = 0;
        normalTextDraws = 0;
        preparedMaterials.clear();
    }

    std::vector<FrozenFrameRequest> requests;
    std::vector<std::uint64_t> capturedSessions;
    std::vector<core::FrozenBackdrop> preparedMaterials;
    std::vector<SurfaceTag> cleanRects;
    std::vector<SurfaceTag> normalRects;
    int captures = 0;
    int cleanTextDraws = 0;
    int normalTextDraws = 0;
    bool requestStripCapture = false;
    core::FrozenBackdrop capturedStrip;
    std::vector<FrozenFrameRequest> stripRequests;
    std::vector<SurfaceTag> stripCopyRects;
    bool frozenPoisoned = false;

private:
    void invalidateSession() {
        if (!frozenPoisoned) {
            frozenPoisoned = true;
            ++core::render::currentRenderFrameStats().frozenInvalidations;
        }
        captureActive = false;
    }

    bool captureActive = false;
    bool settling = false;
    int settleFrames = 0;
    std::uint64_t lastSession = 0;
    std::uint64_t lastRevision = 0;
    int lastWidth = 0;
    int lastHeight = 0;
    float lastDpi = 1.0f;
};

void composeFixture(core::dsl::Runtime& runtime,
                    bool menuVariant,
                    bool tooltipVariant,
                    const std::string& backdropText,
                    const std::string& backdropDirtyKey,
                    std::uint64_t session) {
    runtime.compose("frozen-backdrop-runtime", 640.0f, 480.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.rect("backdrop.surface").position(50.0f, 50.0f).size(80.0f, 36.0f)
            .color({0.18f, 0.24f, 0.32f, 1.0f}).build();
        ui.text("backdrop.label").position(52.0f, 52.0f).size(120.0f, 24.0f)
            .text(backdropText).dirtyKey(backdropDirtyKey).color({1, 1, 1, 1}).build();

        ui.stack("menu.root").position(0.0f, 0.0f).size(640.0f, 480.0f)
            .zIndex(900).excludeFromBackdropSnapshot()
            .content([&] {
                const float menuX = menuVariant ? 112.0f : 100.0f;
                ui.rect("menu.surface").position(menuX, 90.0f).size(180.0f, 170.0f)
                    .color({0.12f, 0.14f, 0.17f, 1.0f})
                    .frozenBackdrop(core::FrozenBackdrop{session, 501, {}, 8.0f, 0.78f}).build();
                ui.text("menu.label").position(menuX + 8.0f, 98.0f).size(150.0f, 24.0f)
                    .text(menuVariant ? "Menu changed" : "Menu original")
                    .dirtyKey(menuVariant ? "menu-b" : "menu-a")
                    .color({1, 1, 1, 1}).build();
            }).build();

        ui.stack("topbar.root").position(0.0f, 0.0f).size(640.0f, 32.0f)
            .zIndex(2000)
            .content([&] {
                ui.rect("topbar.surface").position(10.0f, 2.0f).size(60.0f, 24.0f)
                    .color({0.22f, 0.28f, 0.36f, 1.0f}).build();
                ui.text("topbar.label").position(12.0f, 4.0f).size(56.0f, 20.0f)
                    .text("File").color({1, 1, 1, 1}).build();
            }).build();

        ui.stack("tooltip.root").position(0.0f, 0.0f).size(640.0f, 480.0f)
            .zIndex(3000).excludeFromBackdropSnapshot()
            .content([&] {
                const float tooltipX = tooltipVariant ? 224.0f : 210.0f;
                ui.rect("tooltip.surface").position(tooltipX, 28.0f).size(90.0f, 26.0f)
                    .color({0.30f, 0.30f, 0.30f, 1.0f}).build();
                ui.text("tooltip.label").position(tooltipX + 2.0f, 30.0f).size(84.0f, 20.0f)
                    .text(tooltipVariant ? "Tooltip B" : "Tooltip A")
                    .dirtyKey(tooltipVariant ? "tip-b" : "tip-a")
                    .color({1, 1, 1, 1}).build();
            }).build();
    });
    runtime.update(nullptr, 0.0f, 1.0f, 1.5f);
}

void composeSignatureFixture(core::dsl::Runtime& runtime,
                             std::uint64_t session,
                             const std::string& containerDirtyKey,
                             const std::string& childText,
                             bool caretPresent) {
    runtime.compose("frozen-backdrop-signature", 640.0f, 480.0f,
                    [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.stack("signature.background").position(0.0f, 0.0f).size(640.0f, 480.0f)
            .dirtyKey(containerDirtyKey)
            .content([&] {
                ui.rect("signature.surface").position(50.0f, 50.0f).size(80.0f, 36.0f)
                    .color({0.18f, 0.24f, 0.32f, 1.0f}).build();
                ui.text("signature.label").position(52.0f, 52.0f).size(120.0f, 24.0f)
                    .text(childText).color({1, 1, 1, 1}).build();
                if (caretPresent) {
                    ui.rect("input.cursor").position(52.0f, 52.0f).size(1.0f, 18.0f)
                        .excludeFromBackdropSnapshot().color({1, 1, 1, 1}).build();
                }
            }).build();
        ui.rect("signature.menu").position(100.0f, 90.0f).size(180.0f, 170.0f)
            .color({0.12f, 0.14f, 0.17f, 1.0f})
            .frozenBackdrop(core::FrozenBackdrop{session, 601, {}, 8.0f, 0.78f}).build();
    });
    runtime.update(nullptr, 0.0f, 1.0f, 1.5f);
}

struct SettledRenderResult {
    int frames = 0;
    int deferredFrames = 0;
};

SettledRenderResult renderUntilFrozenSettles(core::dsl::Runtime& runtime,
                                             FrozenBackdropRecorder& backend,
                                             const core::Color& clear,
                                             int maximumFrames = 5) {
    SettledRenderResult result;
    for (; result.frames < maximumFrames; ++result.frames) {
        backend.resetFrameObservations();
        runtime.render(960, 720, 1.5f, clear);
        if (core::render::lastRenderFrameStats().frozenCaptureDeferred) {
            ++result.deferredFrames;
            continue;
        }
        ++result.frames;
        return result;
    }
    return result;
}

void testCleanPrepassAndNormalOrdering(core::dsl::Runtime& runtime,
                                      FrozenBackdropRecorder& backend) {
    composeFixture(runtime, false, false, "Backdrop A", "base-a", 1001);
    const SettledRenderResult settled = renderUntilFrozenSettles(
        runtime, backend, {0.04f, 0.05f, 0.06f, 1.0f});

    check(!backend.requests.empty(), "Runtime asks the backend to prepare a frozen scene");
    if (backend.requests.empty()) return;
    const FrozenFrameRequest& request = backend.requests.back();
    check(request.session == 1001, "Runtime forwards the material session to beginFrozenBackdropFrame");
    check(request.sceneRevision != 0, "active frozen session has a nonzero scene signature");
    check(request.width == 960 && request.height == 720 && near(request.dpiScale, 1.5f),
          "Runtime forwards framebuffer dimensions and DPI");
    check(settled.frames == 2 && settled.deferredFrames == 1,
          "a stable opening defers one full frame before capture");
    check(backend.captures == 1 && !backend.capturedSessions.empty() &&
          backend.capturedSessions.back() == 1001,
          "clean prepass capture is associated with the active session");

    check(backend.cleanRects.size() == 2 &&
          backend.cleanRects[0] == SurfaceTag::Backdrop &&
          backend.cleanRects[1] == SurfaceTag::TopBar,
          "clean prepass excludes menu and tooltip while including higher-z top bar");
    check(backend.cleanTextDraws > 0 && backend.cleanTextDraws < backend.normalTextDraws,
          "clean prepass includes underlying text but excludes menu/tooltip text");

    check(backend.normalRects.size() == 4 &&
          backend.normalRects[0] == SurfaceTag::Backdrop &&
          backend.normalRects[1] == SurfaceTag::Menu &&
          backend.normalRects[2] == SurfaceTag::TopBar &&
          backend.normalRects[3] == SurfaceTag::Tooltip,
          "ordinary pass retains base, menu, top-bar, tooltip z-order");
    check(backend.normalTextDraws > backend.cleanTextDraws,
          "ordinary pass still renders text from the excluded menu and tooltip");

    check(backend.preparedMaterials.size() == 1 &&
          backend.preparedMaterials[0].session == 1001 &&
          backend.preparedMaterials[0].panel == 501,
          "Runtime forwards the frozen material session/panel to backend preparation");
    if (!backend.preparedMaterials.empty()) {
        const auto& bounds = backend.preparedMaterials[0].sourceBounds;
        check(near(bounds.x, 150.0f) && near(bounds.y, 135.0f) &&
              near(bounds.width, 270.0f) && near(bounds.height, 255.0f),
              "Runtime converts source bounds to final framebuffer pixels");
        check(near(backend.preparedMaterials[0].sigma, 12.0f),
              "Runtime scales frozen filter sigma by DPI");
    }
}

void testSceneSignatureExclusionsAndInvalidation(core::dsl::Runtime& runtime,
                                                  FrozenBackdropRecorder& backend) {
    const std::uint64_t initialRevision = backend.requests.back().sceneRevision;
    const int capturesBefore = backend.captures;

    composeFixture(runtime, true, true, "Backdrop A", "base-a", 1001);
    backend.resetFrameObservations();
    runtime.render(960, 720, 1.5f, {0.04f, 0.05f, 0.06f, 1.0f});
    check(backend.requests.back().sceneRevision == initialRevision,
          "menu and tooltip content/geometry changes do not alter clean-scene signature");
    check(backend.captures == capturesBefore,
          "excluded menu/tooltip changes do not request another clean-scene capture");

    composeFixture(runtime, true, true, "Backdrop B", "base-a", 1001);
    backend.resetFrameObservations();
    runtime.render(960, 720, 1.5f, {0.04f, 0.05f, 0.06f, 1.0f});
    const std::uint64_t textRevision = backend.requests.back().sceneRevision;
    check(textRevision != initialRevision,
          "changing included backdrop text changes the clean-scene signature");
    check(backend.captures == capturesBefore && backend.frozenPoisoned &&
          core::render::lastRenderFrameStats().frozenInvalidations == 1,
          "changed included text invalidates the opening without recapturing");

    composeFixture(runtime, true, true, "Backdrop B", "base-b", 1001);
    backend.resetFrameObservations();
    runtime.render(960, 720, 1.5f, {0.04f, 0.05f, 0.06f, 1.0f});
    const std::uint64_t dirtyKeyRevision = backend.requests.back().sceneRevision;
    check(dirtyKeyRevision != textRevision,
          "changing included backdrop dirtyKey changes the clean-scene signature");
    check(backend.captures == capturesBefore && backend.frozenPoisoned &&
          core::render::lastRenderFrameStats().frozenInvalidations == 0,
          "continued scene changes stay invalidated without duplicate capture or invalidation counts");
}

void testSessionForwarding(core::dsl::Runtime& runtime,
                           FrozenBackdropRecorder& backend) {
    const int capturesBefore = backend.captures;
    composeFixture(runtime, true, true, "Backdrop B", "base-b", 2002);
    const SettledRenderResult settled = renderUntilFrozenSettles(
        runtime, backend, {0.04f, 0.05f, 0.06f, 1.0f});
    check(backend.requests.back().session == 2002,
          "new material session is forwarded to backend frame setup");
    check(backend.captures == capturesBefore + 1 &&
          backend.capturedSessions.back() == 2002,
          "new session triggers a session-bound clean snapshot");
    check(settled.frames == 2 && settled.deferredFrames == 1,
          "session forwarding retains the opening stabilization frame");
    check(backend.preparedMaterials.size() == 1 && backend.preparedMaterials[0].session == 2002,
          "new session reaches panel preparation unchanged");
}

void testContainerDirtyKeyAndExcludedCaret(core::dsl::Runtime& runtime,
                                           FrozenBackdropRecorder& backend) {
    constexpr std::uint64_t session = 2502;
    const core::Color clear{0.04f, 0.05f, 0.06f, 1.0f};
    composeSignatureFixture(runtime, session, "container-a", "Backdrop", false);
    const SettledRenderResult settled = renderUntilFrozenSettles(runtime, backend, clear);
    const std::uint64_t initialRevision = backend.requests.back().sceneRevision;
    const int initialCaptures = backend.captures;
    check(settled.frames == 2 && settled.deferredFrames == 1,
          "container/caret fixture captures only after the stable observation");

    composeSignatureFixture(runtime, session, "container-b", "Backdrop", false);
    runtime.render(960, 720, 1.5f, clear);
    check(backend.requests.back().sceneRevision == initialRevision &&
          !backend.frozenPoisoned && backend.captures == initialCaptures,
          "container-only dirtyKey changes do not invalidate the frozen scene");

    composeSignatureFixture(runtime, session, "container-b", "Changed backdrop", false);
    runtime.render(960, 720, 1.5f, clear);
    const std::uint64_t childTextRevision = backend.requests.back().sceneRevision;
    check(childTextRevision != initialRevision && backend.frozenPoisoned &&
          backend.captures == initialCaptures,
          "painted descendant text still invalidates despite ignored container dirtyKey");

    composeSignatureFixture(runtime, session, "container-b", "Changed backdrop", true);
    runtime.render(960, 720, 1.5f, clear);
    check(backend.requests.back().sceneRevision == childTextRevision &&
          backend.captures == initialCaptures,
          "inserting excluded input.cursor does not change scene signature or recapture");
    composeSignatureFixture(runtime, session, "container-b", "Changed backdrop", false);
    runtime.render(960, 720, 1.5f, clear);
    check(backend.requests.back().sceneRevision == childTextRevision &&
          backend.captures == initialCaptures,
          "removing excluded input.cursor does not change scene signature or recapture");
}

void testClearColorInvalidatesUntilNewSession(core::dsl::Runtime& runtime,
                                              FrozenBackdropRecorder& backend) {
    const core::Color initialClear{0.04f, 0.05f, 0.06f, 1.0f};
    composeFixture(runtime, false, false, "Backdrop C", "base-c", 3003);
    const SettledRenderResult settled = renderUntilFrozenSettles(runtime, backend, initialClear);
    const int capturesAfterOpen = backend.captures;
    const std::uint64_t initialRevision = backend.requests.back().sceneRevision;
    check(settled.frames == 2 && settled.deferredFrames == 1 && !backend.frozenPoisoned,
          "new session stabilizes and captures a valid clean snapshot");

    const core::Color changedClear{0.08f, 0.07f, 0.06f, 1.0f};
    runtime.requestFullPaint();
    backend.resetFrameObservations();
    runtime.render(960, 720, 1.5f, changedClear);
    const std::uint64_t changedRevision = backend.requests.back().sceneRevision;
    check(changedRevision != initialRevision,
          "Runtime mixes clearColor into sceneRevision sent to the backend");
    check(backend.frozenPoisoned &&
          core::render::lastRenderFrameStats().frozenInvalidations == 1,
          "clearColor change invalidates the frozen opening");
    check(backend.captures == capturesAfterOpen,
          "clearColor invalidation does not recapture during the same session");

    composeFixture(runtime, false, false, "Backdrop C", "base-c", 3004);
    backend.resetFrameObservations();
    const auto reopened = renderUntilFrozenSettles(runtime, backend, changedClear);
    check(reopened.frames == 2 && reopened.deferredFrames == 1,
          "reopened session observes one settling frame before capture");
    check(!backend.frozenPoisoned && backend.captures == capturesAfterOpen + 1 &&
          backend.capturedSessions.back() == 3004,
          "a new session clears invalidation and captures with the new clearColor");
}

void testPreCaptureFocusHandoffAndBoundedFallback(core::dsl::Runtime& runtime,
                                                   FrozenBackdropRecorder& backend) {
    constexpr std::uint64_t changingSession = 3303;
    const core::Color clear{0.04f, 0.05f, 0.06f, 1.0f};
    const int capturesBefore = backend.captures;
    composeFixture(runtime, false, false, "Focus before open", "focus-a", changingSession);
    runtime.render(960, 720, 1.5f, clear);
    check(core::render::lastRenderFrameStats().frozenCaptureDeferred &&
          backend.captures == capturesBefore && !backend.frozenPoisoned,
          "new opening defers capture without treating the first observation as a capture");

    composeFixture(runtime, false, false, "Focus settled", "focus-b", changingSession);
    runtime.render(960, 720, 1.5f, clear);
    const std::uint64_t settledRevision = backend.requests.back().sceneRevision;
    check(core::render::lastRenderFrameStats().frozenCaptureDeferred &&
          settledRevision != backend.requests[backend.requests.size() - 2].sceneRevision &&
          !backend.frozenPoisoned && backend.captures == capturesBefore,
          "focus/paint change before capture updates the pending identity and defers again");

    runtime.render(960, 720, 1.5f, clear);
    check(!core::render::lastRenderFrameStats().frozenCaptureDeferred &&
          !backend.frozenPoisoned && backend.captures == capturesBefore + 1 &&
          backend.capturedSessions.back() == changingSession,
          "stable post-focus observation captures exactly once without poisoning");
    composeFixture(runtime, false, false, "Changed after capture", "focus-c", changingSession);
    runtime.render(960, 720, 1.5f, clear);
    check(backend.frozenPoisoned && backend.captures == capturesBefore + 1,
          "a scene change after capture still poisons without recapturing");

    constexpr std::uint64_t unstableSession = 3404;
    const int unstableCapturesBefore = backend.captures;
    composeSignatureFixture(runtime, unstableSession, "container", "Unstable 0", false);
    runtime.render(960, 720, 1.5f, clear);
    int deferredCount = core::render::lastRenderFrameStats().frozenCaptureDeferred ? 1 : 0;
    for (int revision = 1; revision <= 3; ++revision) {
        composeSignatureFixture(runtime, unstableSession, "container",
                                "Unstable " + std::to_string(revision), false);
        runtime.render(960, 720, 1.5f, clear);
        if (core::render::lastRenderFrameStats().frozenCaptureDeferred) ++deferredCount;
    }
    check(deferredCount == 3 && backend.frozenPoisoned &&
          !core::render::lastRenderFrameStats().frozenCaptureDeferred &&
          backend.captures == unstableCapturesBefore,
          "continually changing scene falls back after at most three deferred frames");
    composeSignatureFixture(runtime, unstableSession, "container", "Stable after fallback", false);
    runtime.render(960, 720, 1.5f, clear);
    check(backend.frozenPoisoned && backend.captures == unstableCapturesBefore,
          "a poisoned unstable opening stays on solid fallback until reopened");
}

} // namespace

void testStripCleanSourceAndIndependentSession(core::dsl::Runtime& runtime,
                                               FrozenBackdropRecorder& backend) {
    backend.resetFrameObservations();
    backend.requestStripCapture = true;
    core::dsl::Ui* composedUi = nullptr;
    runtime.compose("strip-clean-source", 640, 480, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        composedUi = &ui;
        ui.rect("source.body").position(50, 50).size(400, 100).color({0.2f, 0.3f, 0.4f, 1}).build();
        ui.rect("source.caret").position(112, 65).size(1, 20)
            .excludeFromBackdropSnapshot().color({0, 0, 0, 1}).build();
        ui.stack("chrome").size(640, 32).zIndex(2000).excludeFromBackdropSnapshot().content([&] {
            core::FrozenBackdrop strip{8001, 1, {}, 6, 0.58f};
            strip.cachedStrip = true; strip.sourceOffset = {0, 50};
            ui.rect("chrome.surface").position(10, 2).size(420, 32).color({0.9f, 0.9f, 0.9f, 1})
                .frozenBackdrop(strip).build();
        }).build();
    });
    check(composedUi->cachedStripSession() == 8001 && composedUi->frozenBackdropSession() == 0,
          "persistent chrome has a separate session from transient menus");
    runtime.update(nullptr, 0, 1, 1.5f);
    runtime.requestFullPaint();
    runtime.render(960, 720, 1.5f, {1, 1, 1, 1});
    check(backend.capturedStrip.session == 8001 && near(backend.capturedStrip.sourceBounds.x, 15) &&
          near(backend.capturedStrip.sourceBounds.y, 78) && near(backend.capturedStrip.sourceBounds.width, 630) &&
          near(backend.capturedStrip.sigma, 9), "strip source offset and Gaussian radius use framebuffer DPI");
    const auto contains = [](const std::vector<SurfaceTag>& values, SurfaceTag tag) {
        return std::find(values.begin(), values.end(), tag) != values.end();
    };
    check(contains(backend.stripCopyRects, SurfaceTag::Backdrop) &&
          !contains(backend.stripCopyRects, SurfaceTag::Menu) &&
          !contains(backend.stripCopyRects, SurfaceTag::TopBar),
          "strip copies a clean source band without the excluded caret or chrome");
    check(contains(backend.normalRects, SurfaceTag::Menu) && contains(backend.normalRects, SurfaceTag::TopBar),
          "the final normal pass restores the caret and chrome after source capture");
    backend.resetFrameObservations();
    runtime.compose("strip-short-viewport", 320, 100, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
        core::FrozenBackdrop strip{8002, 1, {}, 6, 0.58f};
        strip.cachedStrip = true; strip.sourceOffset = {0, 120};
        ui.rect("chrome.too.low").size(320, 32).frozenBackdrop(strip).build();
    });
    runtime.update(nullptr, 0, 1, 1);
    runtime.requestFullPaint();
    runtime.render(320, 100, 1, {1, 1, 1, 1});
    check(backend.capturedStrip.session == 8002 && !backend.capturedStrip.allowCapture &&
          backend.stripCopyRects.empty(), "a source below the viewport is rejected before any clean scissor pass");
    backend.requestStripCapture = false;
}

int main() {
    FrozenBackdropRecorder backend;
    core::render::ScopedRenderBackend scoped{backend};
    core::dsl::Runtime runtime;
    check(runtime.initialize(), "runtime initializes without a native window");
    if (failures != 0) return 1;

    testCleanPrepassAndNormalOrdering(runtime, backend);
    testSceneSignatureExclusionsAndInvalidation(runtime, backend);
    testSessionForwarding(runtime, backend);
    testContainerDirtyKeyAndExcludedCaret(runtime, backend);
    testClearColorInvalidatesUntilNewSession(runtime, backend);
    testPreCaptureFocusHandoffAndBoundedFallback(runtime, backend);
    testStripCleanSourceAndIndependentSession(runtime, backend);
    runtime.shutdown();
    return failures == 0 ? 0 : 1;
}
