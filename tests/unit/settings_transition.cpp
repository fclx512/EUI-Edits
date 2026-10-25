#include "../../apps/neo_editor/ui/settings_transition.h"
#include "core/dsl_runtime.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Recorder final : core::render::RenderBackend {
    float opacity = -1;
    float editorOpacity = -1;
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int,int) override { return true; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int,int,const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int,int,core::render::RenderCacheBlitMode,const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool,const core::Rect&,int) override {}
    void prepareBackdropBlur(const core::Rect&,float,int,int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& c,int,int) override {
        if (!c.shadowPass) {
            if (c.fillColor.r > .8f) opacity=c.opacity;
            else editorOpacity=c.opacity;
        }
    }
    void drawPolygon(const core::render::PolygonDrawCommand&,int,int) override {}
    void drawText(const core::render::TextDrawCommand&,int,int) override {}
};

void run(int hz) {
    Recorder backend; core::render::ScopedRenderBackend scoped(backend);
    core::dsl::Runtime runtime; check(runtime.initialize(),"initialize");
    int tag=0; auto window=reinterpret_cast<core::window::Handle>(&tag);
    bool visible=false,animations=true;
    int renders=0;
    core::dsl::Ui* view=nullptr;
    const auto step=[&](float dt) {
        runtime.compose("settings-entry",640,480,[&](core::dsl::Ui& ui,const core::dsl::Screen& screen) {
            view=&ui;
            neo::settingsTransitionView(ui,screen,visible,animations,[&] {
                ++renders;
                ui.rect("settings-body").size(640,480).color({.9f,.91f,.92f,1}).onClick([]{}).build();
            });
            neo::editorTransitionView(ui,screen,!visible,animations,[&] {
                ui.rect("editor-body").size(640,480).color({.4f,.41f,.42f,1}).onClick([]{}).build();
            });
        });
        bool paint=runtime.update(window,dt,1,1);
        if(paint) runtime.render(640,480,1,{1,1,1,1});
        return paint;
    };
    step(0);
    check(renders==0 && !view->find("settings-body"),"closed entry composed the expensive settings tree");
    check(core::closeEnough(backend.editorOpacity,1.0f),"initial editor unexpectedly animates from blank");
    visible=true; step(.04f);
    check(backend.opacity>0 && backend.opacity<1,"first entry skipped its transition");
    check(!view->find("editor-body"),"settings retained a live editor tree");
    step(.08f);
    check(backend.opacity<.999f,"settings entry still finishes at the old 120 ms duration");
    int paints=2,fast=0;
    for(int i=0;i<hz/2;++i) { paints+=step(1.0f/hz)?1:0; fast+=runtime.isAnimating()?1:0; }
    check(paints<=15 && fast==0,"settings entry followed the display refresh rate");
    check(core::closeEnough(backend.opacity,1.0f),"entry did not reach its final state");
    check(!step(.1f) && std::isinf(runtime.nextTimerWakeSeconds()),"settled entry still wakes");
    visible=false; step(.04f);
    check(!view->find("settings-body") && view->find("settings.transition")->disabled,
          "closed settings retained a live body");
    check(backend.editorOpacity>0 && backend.editorOpacity<1,"return to editor skipped its transition");
    step(.08f);
    check(backend.editorOpacity<.999f,"editor return finishes before the requested 200 ms");
    paints=2;fast=0;
    for(int i=0;i<hz/2;++i) { paints+=step(1.0f/hz)?1:0; fast+=runtime.isAnimating()?1:0; }
    check(paints<=15 && fast==0,"editor return followed the display refresh rate");
    check(core::closeEnough(backend.editorOpacity,1.0f),"editor return did not settle");
    check(!step(.1f) && std::isinf(runtime.nextTimerWakeSeconds()),"editor return left an idle timer");
    visible=true; step(.02f); visible=false; step(0); visible=true; step(.02f);
    animations=false; step(0);
    check(core::closeEnough(backend.opacity,1.0f) && !runtime.isAnimating(),"animation-off did not finish a rapid reopen");
    check(!step(.1f) && std::isinf(runtime.nextTimerWakeSeconds()),"animation-off left an entry timer");
    runtime.shutdown();core::releaseInputQueue(window);
}
}

int main() {
    try { run(240);run(1000); }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n';return 1; }
    std::cout<<"settings/editor 200 ms entries, 60 Hz, off, rapid reopen and idle passed\n";
}
