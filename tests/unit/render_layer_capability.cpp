#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

class NoRetainedLayersBackend final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    bool supportsRetainedLayers() const override { return false; }
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
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {
        ++roundedRectDraws;
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}

    LayerHandle createLayer(int, int) override {
        ++layerCreateCalls;
        return nullptr;
    }

    int roundedRectDraws = 0;
    int layerCreateCalls = 0;
};

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

void composeLayerCandidate(core::dsl::Runtime& runtime) {
    runtime.compose("render-layer-capability", 160.0f, 120.0f,
                    [](core::dsl::Ui& ui, const core::dsl::Screen&) {
        ui.stack("root").size(160.0f, 120.0f).content([&] {
            ui.stack("candidate").position(10.0f, 10.0f).size(120.0f, 90.0f).content([&] {
                for (int index = 0; index < 9; ++index) {
                    ui.rect("rect." + std::to_string(index))
                        .position(static_cast<float>((index % 3) * 35),
                                  static_cast<float>((index / 3) * 25))
                        .size(30.0f, 20.0f)
                        .build();
                }
            }).build();
        }).build();
    });
}

} // namespace

int main() {
    NoRetainedLayersBackend backend;
    core::render::ScopedRenderBackend activeBackend(backend);
    core::dsl::Runtime runtime;
    composeLayerCandidate(runtime);

    bool ok = true;
    int previousDrawCount = 0;
    for (int frame = 0; frame < 4; ++frame) {
        // Runtime::update builds paint bounds from the pointer event traversal. A null
        // window keeps this a headless test; the disabled-input path places the event
        // outside the content and never interacts with a control.
        runtime.update(nullptr, 0.0f, 1.0f, 1.0f, false);
        runtime.requestFullPaint();
        runtime.render(160, 120, 1.0f, {0.12f, 0.14f, 0.16f, 1.0f});

        const auto& stats = core::render::lastRenderFrameStats();
        ok &= check(stats.retainedLayerMisses == 0,
                    "unsupported retained layers caused a warmup miss");
        ok &= check(backend.layerCreateCalls == 0,
                    "unsupported retained layers called createLayer");
        ok &= check(backend.roundedRectDraws >= previousDrawCount + 9,
                    "UI rectangles were not drawn through the ordinary path");
        ok &= check(!runtime.paintRequested(),
                    "unsupported retained layers kept requesting warmup frames");
        previousDrawCount = backend.roundedRectDraws;
    }

    runtime.shutdown(false);
    return ok ? 0 : 1;
}
