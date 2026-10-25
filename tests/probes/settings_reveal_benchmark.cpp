// Retained-runtime fixture, synthetic pointer queue and recording backend.
// No OS pointer injection, D2D rasterization or display latency is measured here.
#include "../../apps/neo_editor/ui/interaction_reveal.h"
#include "core/dsl_runtime.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
class Recorder final : public core::render::RenderBackend {
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
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& cmd, int, int) override {
        ++draws;
        if(cmd.reveal.enabled) last=cmd.reveal;
        if (cmd.rect.x == 400.0f && cmd.rect.y == 280.0f) first = cmd.reveal;
        if (cmd.rect.x == 400.0f && cmd.rect.y == 360.0f) second = cmd.reveal;
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
    int draws = 0;
    core::RevealEffect last{};
    core::RevealEffect first{}, second{};
};
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Fixture {
    Recorder backend;
    core::render::ScopedRenderBackend scoped{backend};
    core::dsl::Runtime runtime;
    core::window::Handle window = nullptr; // Runtime model only; no native window/input.
    int composeRequests = 0, paints = 0, fullPaints = 0;
    std::uint64_t dirtyPixels = 0;
    int extraButtons = 0;
    Fixture(bool enabled, int extra = 0) : extraButtons(extra) { runtime.initialize(); compose(enabled); }
    ~Fixture() { runtime.shutdown(); core::releaseInputQueue(window); }
    void compose(bool enabled, float offset = 0.0f, core::Color accent = {.2f,.5f,.9f,1}) {
        runtime.compose("reveal-fixture", 640, 480, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            for (int i=0;i<100;++i) ui.rect("static"+std::to_string(i))
                .position(float((i%10)*30),float((i/10)*24)).size(20,16).color({.1f,.1f,.1f,1}).build();
            neo::EditorColors colors{}; colors.accent=accent;
            ui.stack("buttons").position(0,offset).size(640,480).content([&] {
                for(int i=0;i<2;++i) ui.rect("button"+std::to_string(i))
                    .position(400,float(280+i*80)).size(176,34).color({.15f,.15f,.15f,1})
                    .reveal(neo::interaction_reveal::effect(colors,enabled)).onClick([]{}).build();
            }).build();
            for (int i=0;i<extraButtons;++i) ui.rect("dense"+std::to_string(i))
                .position(12.0f+float(i%10)*60.0f,float(i/10)*38.0f).size(52,28)
                .color({.15f,.15f,.15f,1})
                .reveal(neo::interaction_reveal::effect(colors,enabled)).onClick([]{}).build();
        });
    }
    bool step(float x,float y,float dt,bool input=true) {
        core::queuePointerMotion(window,x,y,{},{});
        bool changed=runtime.update(window,dt,1,1,input);
        if(runtime.composeRequested()) ++composeRequests;
        if(changed) {
            ++paints;
            runtime.render(640,480,1,core::Color{.08f,.08f,.08f,1});
            const auto stats=core::render::lastRenderFrameStats();
            if(stats.fullPaint) ++fullPaints;
            dirtyPixels+=stats.dirtyPixels;
        }
        return changed;
    }
    void settle(float x,float y) { for(int i=0;i<30;++i) step(x,y,1.0f/60); }
    void resetCounters() { paints=fullPaints=composeRequests=backend.draws=0; dirtyPixels=0; }
};
void contracts() {
    Fixture f(true);
    f.settle(440,290);
    check(f.backend.first.enabled && f.backend.first.fillOpacity>0,"hover surface lights");
    f.resetCounters();
    f.settle(440,330);
    check(f.backend.first.enabled && f.backend.second.enabled,"outside pointer lights both neighboring borders");
    check(f.backend.first.fillOpacity==0 && f.backend.second.fillOpacity==0,"unhovered neighbors have no surface light");
    check(f.composeRequests==0 && f.fullPaints==0,"lighting only damages retained regions without compose/full paint");
    f.settle(10,10); f.resetCounters();
    for(int i=0;i<100;++i) f.step(float(i),10,.001f);
    check(f.paints==0 && !std::isfinite(f.runtime.nextTimerWakeSeconds()),"far pointer causes no paints or wakes");
    f.settle(440,290); f.resetCounters();
    for(int i=0;i<256;++i) f.step(440,290,.001f);
    check(f.paints==0 && !std::isfinite(f.runtime.nextTimerWakeSeconds()),"settled stationary pointer sleeps");
    f.step(440,290,.001f,false);
    check(!f.backend.first.enabled && !std::isfinite(f.runtime.nextTimerWakeSeconds()),"input disabled clears retained lighting");
    f.settle(440,290); f.compose(false); f.step(440,290,0);
    check(!f.backend.first.enabled,"style/animation disable clears lighting");
    f.compose(true,0,{.9f,.3f,.1f,.5f}); f.settle(440,290);
    check(f.backend.first.tint.r==.9f && f.backend.first.tint.a==.5f,"theme including alpha reaches draw command");
    // Production coordinate conversion: layout in DIPs, pointer in pixels,
    // translated ancestor, then D2D command in pixels at 150% UI scale.
    f.runtime.compose("scaled-fixture",640,480,[&](core::dsl::Ui& ui,const core::dsl::Screen&) {
        neo::EditorColors colors{}; colors.accent={.2f,.6f,.9f,1};
        ui.stack("parent").position(0,0).size(640,480).translate(30,20).content([&] {
            ui.rect("scaled-button").position(100,100).size(176,34)
              .color({.15f,.15f,.15f,1}).reveal(neo::interaction_reveal::effect(colors,true))
              .onClick([]{}).build();
        }).build();
    });
    for(int i=0;i<30;++i) {
        core::queuePointerMotion(f.window,210,195,{},{}); // local=(10,10) DIP
        f.runtime.update(f.window,1.0f/60,1,1.5f);
        f.runtime.render(960,720,1.5f,core::Color{.1f,.1f,.1f,1});
    }
    check(std::fabs(f.backend.last.center.x-15)<.001f &&
          std::fabs(f.backend.last.center.y-15)<.001f &&
          std::fabs(f.backend.last.radius-108)<.001f,
          "ancestor transform and DPI preserve pointer/light alignment");

}
void trial(bool enabled,int round, int extra = 0) {
    Fixture f(enabled,extra); f.settle(extra ? 20.0f : 440.0f, extra ? 190.0f : 290.0f); f.resetCounters();
    const auto start=std::chrono::steady_clock::now();
    for(int i=1;i<=1000;++i) f.step(extra ? 20.0f+float(i%570) : 420.0f+float(i%140),extra ? 190.0f : 290.0f,.001f);
    const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    check(f.composeRequests==0 && f.fullPaints==0,"sampled motion does not compose or full paint");
    check(f.paints<=61,"1000Hz input produces at most 61 visual updates per simulated second");
    if(enabled) check(f.paints>=50,"continuous motion produces useful visual updates");
    else check(f.paints==0,"effect off has no pointer paints");
    std::cout<<"{\"round\":"<<round<<",\"extra_buttons\":"<<extra<<",\"enabled\":"<<(enabled?"true":"false")
      <<",\"input_samples\":1000,\"simulated_seconds\":1,\"wall_us\":"<<us
      <<",\"paints\":"<<f.paints<<",\"compose_requests\":"<<f.composeRequests
      <<",\"full_paints\":"<<f.fullPaints<<",\"rect_draws\":"<<f.backend.draws
      <<",\"dirty_pixels\":"<<f.dirtyPixels<<"}\n";
    f.settle(440,290);
    if(enabled) check(std::fabs(f.backend.first.center.x-40)<.001f,"latest pointer reaches renderer after deadline");
}
}
int main(int argc,char** argv) {
    contracts();
    const bool dense=argc>1 && std::string(argv[1])=="--dense";
    const bool bench=dense || (argc>1 && std::string(argv[1])=="--benchmark");
    const int rounds=bench?3:1;
    for(int r=0;r<rounds;++r) { trial(false,r,dense?120:0);trial(true,r,dense?120:0);if(bench){trial(true,r,dense?120:0);trial(false,r,dense?120:0);} }
    if (!bench) { trial(false,0,120); trial(true,0,120); }
    return failures?1:0;
}
