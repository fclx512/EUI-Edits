// Same target trajectory at 240/1000 updates/sec. Hidden software D2D is
// optional; neither path measures native input latency or displayed frames.
#include "core/dsl_runtime.h"
#if defined(_WIN32)
#include <windows.h>
#endif
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace {
struct Recorder final : core::render::RenderBackend {
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
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode, const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
};
}

int main(int argc, char** argv) {
    const bool d2d = argc > 1 && std::string(argv[1]) == "--d2d";
    const bool smoke = argc > 1 && std::string(argv[1]) == "--frames";
    const float limit = !smoke && argc > 2 ? std::stof(argv[2]) : 60.0f;
    const int hz = argc > 3 ? std::stoi(argv[3]) : 240;
    const int rounds = argc > 4 ? std::stoi(argv[4]) : 1;
    if (hz < 60 || rounds < 1) return 2;
    Recorder recorder;
    core::window::Handle window = nullptr;
    std::unique_ptr<core::render::RenderBackend> raster;
#if defined(EUI_RENDER_BACKEND_D2D) && defined(_WIN32)
    if (d2d) {
        SetEnvironmentVariableA("NEO_D2D_SOFTWARE", "1");
        SetEnvironmentVariableA("NEO_WIN32_DC", "1");
        window = static_cast<core::window::Handle>(CreateWindowExW(0, L"STATIC", L"Transition cadence probe",
            WS_POPUP, 0, 0, 1024, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        if (!window) return 2;
        raster = core::render::createRenderBackend(window);
        if (!raster || !raster->initialize()) return 2;
    }
#else
    if (d2d) return 2;
#endif
    int failures = 0;
    {
        core::render::RenderBackend& backend = raster ? *raster : recorder;
        core::render::ScopedRenderBackend scoped(backend);
        for (int round = 0; round < rounds; ++round) {
            core::dsl::Runtime runtime;
            runtime.initialize(); runtime.setDecorationFrameRateLimit(limit);
            int position = 0;
            const auto compose = [&] {
                runtime.compose("transition-cadence-probe", 1024, 720, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                    for (int i = 0; i < 120; ++i)
                        ui.rect("body." + std::to_string(i)).position(float(i % 12) * 80, float(i / 12) * 52)
                            .size(70, 40).color({.95f,.95f,.95f,1}).build();
                    for (int i = 0; i < 60; ++i)
                        ui.text("label." + std::to_string(i)).position(18, 20.0f + i * 10)
                            .size(900, 22).fontSize(14).text("工具界面列表 Chinese and English text 012345").build();
                    ui.rect("menu").position(80, 60).size(260, 320).color({.85f,.86f,.88f,1}).build();
                    ui.rect("highlight").position(88, 74.0f + position * 28).size(244, 28)
                        .color({.7f,.73f,.78f,1})
                        .transition(core::Transition::make(.075f).animate(core::AnimProperty::Frame))
                        .animate(core::AnimProperty::Frame).build();
                    ui.stack("popup-position").position(400, 60).size(240, 300)
                        .translateY(position == 0 ? 0.0f : 4.0f)
                        .transition(core::Transition::make(.12f).animate(core::AnimProperty::Transform))
                        .content([&] { ui.rect("panel").fill().color({.88f,.89f,.90f,1}).build(); }).build();
                });
            };
            const auto step = [&](float dt) {
                const bool paint = runtime.update(window, dt, 1, 1);
                if (paint) {
                    backend.beginFrame({window, core::window::nativeWindowInfo(window),1024,720,1});
                    runtime.render(1024,720,1,{1,1,1,1}); backend.present();
                }
                return paint;
            };
            compose(); step(0);
            for (int i=0;i<hz/2;++i) step(1.0f/hz);
            int paints=0, fast=0, quiet=0;
            const auto start=std::chrono::steady_clock::now();
            for (int i=0;i<hz;++i) {
                const int next=(i*16/hz)%2;
                if (next!=position) { position=next; compose(); }
                paints += step(1.0f/hz) ? 1 : 0;
                fast += runtime.isAnimating() ? 1 : 0;
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            for(int i=0;i<hz;++i) step(1.0f/hz);
            for(int i=0;i<hz;++i) quiet += step(1.0f/hz) ? 1 : 0;
            if(quiet || runtime.isAnimating() || !std::isinf(runtime.nextTimerWakeSeconds())) ++failures;
            if(smoke && (paints>61 || fast)) ++failures;
            std::cout<<"{\"backend\":\""<<(d2d?"d2d-software-dc":"recorder")<<"\",\"hz\":"<<hz
                <<",\"limit\":"<<limit<<",\"round\":"<<round<<",\"motion_ms\":"<<ms<<",\"paints\":"<<paints
                <<",\"fast_animation_updates\":"<<fast<<",\"quiet_paints\":"<<quiet<<"}\n";
            runtime.shutdown();core::releaseInputQueue(window);backend.releaseRenderCache();
        }
    }
    raster.reset();
#if defined(_WIN32)
    if(window) DestroyWindow(static_cast<HWND>(window));
#endif
    return failures ? 1 : 0;
}
