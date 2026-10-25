#include "components/contextmenu.h"
#include "core/dsl_runtime.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Recorder final : core::render::RenderBackend {
    std::vector<core::render::RoundedRectDrawCommand> commands;
    int materialRequests = 0;
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
    void prepareBackdropBlur(const core::Rect&, float, int, int) override { ++materialRequests; }
    void prepareFrozenBackdrop(const core::FrozenBackdrop&) override { ++materialRequests; }
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (!command.shadowPass) commands.push_back(command);
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
};

struct Harness {
    int tag = 0;
    core::window::Handle window = reinterpret_cast<core::window::Handle>(&tag);
    Recorder backend;
    core::render::ScopedRenderBackend scoped{backend};
    core::dsl::Runtime runtime;
    core::dsl::Ui* ui = nullptr;
    bool open = false, animations = true, replacement = false, lastPaint = false;
    int dispatches = 0;
    std::vector<int> selected;
    components::theme::ThemeColorTokens tokens = components::theme::light();

    Harness() {
        require(runtime.initialize(), "runtime initialization");
        tokens.surfaceHover = {0.31f, 0.44f, 0.57f, 1};
    }
    ~Harness() { runtime.shutdown(); core::releaseInputQueue(window); }

    void step(float dt, bool forcePaint = true) {
        runtime.compose("menu-motion", 640, 480, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
            ui = &view;
            components::ContextMenuMotion motion;
            if (animations) {
                motion.enter = core::Transition::make(.12f, core::Ease::OutCubic);
                if (!replacement) motion.exit = core::Transition::make(.07f, core::Ease::OutQuad);
                motion.highlight = core::Transition::make(.075f, core::Ease::OutCubic);
            }
            using Item = components::ContextMenuItem;
            components::contextMenu(view, "sample").open(open).screen(640, 480).position(30, 50).size(210, 32)
                .items(std::vector<Item>{Item("Parent", {Item("Child A", {Item("Grandchild")}), Item("Child B")}),
                                        Item("Action 1"), Item("Action 2"), Item("Other Parent", {Item("Other child")})})
                .theme(tokens).motion(motion).skipClosedContent()
                .onSelectPath([&](const std::vector<int>& path) { ++dispatches; selected = path; })
                .onOpenChange([&](bool value) { open = value; }).build();
        });
        require(!ui->frozenBackdropSession() && !ui->cachedStripSession(), "opaque menu requested a cache session");
        lastPaint = runtime.update(window, dt, 1, 1);
        if (forcePaint) { runtime.requestFullPaint(); lastPaint = true; }
        if (lastPaint) {
            backend.commands.clear();
            runtime.render(640, 480, 1, {1, 1, 1, 1});
        }
        require(backend.materialRequests == 0, "opaque menu requested blur preparation");
    }

    core::dsl::Element* element(const std::string& suffix) { return ui->find("sample." + suffix); }
    void hover(int row) {
        auto* item = element("level.0.item." + std::to_string(row));
        require(item && item->onHoverChanged, "hover row missing");
        item->onHoverChanged(true);
    }
    void settle() { for (int i = 0; i < 36; ++i) step(1.0f / 120.0f); }
    core::render::RoundedRectDrawCommand panel(int depth = 0) {
        const auto* bg = element("level." + std::to_string(depth) + ".bg");
        require(bg != nullptr, "panel missing");
        for (const auto& command : backend.commands)
            if (core::closeEnough(command.rect.x, bg->frame.x) &&
                core::closeEnough(command.rect.width, bg->frame.width) &&
                core::closeEnough(command.fillColor, bg->color)) return command;
        throw std::runtime_error("panel draw missing");
    }
    float highlightY() {
        int count = 0;
        float y = 0;
        for (const auto& command : backend.commands) {
            if (core::closeEnough(command.fillColor, tokens.surfaceHover) && command.opacity > .001f) {
                ++count; y = command.rect.y;
            }
        }
        require(count == 1, "highlight must remain a single visible rectangle");
        return y;
    }
};

float spanX(const core::render::RoundedRectDrawCommand& command) {
    float left = std::numeric_limits<float>::infinity(), right = -left;
    for (const auto& vertex : command.vertices) {
        left = std::min(left, vertex.screen.x);
        right = std::max(right, vertex.screen.x);
    }
    return right - left;
}

void lifecycle() {
    Harness h;
    h.step(0); // Prime the closed layout instance before its first open.
    h.open = true;
    h.step(.03f);
    const auto entering = h.panel();
    require(entering.opacity > 0 && entering.opacity < 1, "first open did not animate");
    require(core::closeEnough(h.element("level.0")->transform.scale, core::Vec2{1,1}), "text scale changed");
    h.settle();
    require(core::closeEnough(h.panel().opacity, 1), "enter did not reach opaque endpoint");
    require(core::closeEnough(spanX(entering), spanX(h.panel())), "popup width scaled during entry");

    h.hover(1); h.step(0); h.settle();
    const float start = h.highlightY();
    h.hover(2); h.step(1.0f / 240.0f);
    const float first = h.highlightY();
    const float target = h.element("level.0.highlight")->frame.y;
    require(first > start && first < target, "adjacent highlight did not interpolate");
    h.step(1.0f / 240.0f);
    require(core::closeEnough(h.highlightY(), first), "highlight advanced between 60 Hz samples");
    for (int i=0;i<3;++i) h.step(1.0f/240);
    const float second = h.highlightY();
    require(second > first && second < target, "shared sample snapped or stalled the active highlight");
    h.settle();
    require(core::closeEnough(h.highlightY(), target), "highlight did not settle");
    h.hover(0); h.step(.03f);
    require(core::closeEnough(h.element("level.0.highlight")->frame.y, h.highlightY()), "long jump swept across actions");
    require(h.element("level.1.label.0") != nullptr, "submenu content missing");
    h.settle();
    h.element("level.1.item.0")->onHoverChanged(true); h.step(.03f); h.settle();
    require(h.element("level.2.label.0"), "third level failed to open");
    h.hover(2); h.step(.02f);
    require(h.element("level.1.label.0") && h.element("level.1.item.0")->disabled,
            "closing submenu lost content or retained input");
    const auto childExit = h.panel(1);
    require(childExit.opacity > 0 && childExit.opacity < 1, "submenu exit did not animate");
    h.step(.5f); h.step(0);
    require(!h.element("level.1.label.0") && !h.element("level.2.label.0") &&
            std::isinf(h.runtime.nextTimerWakeSeconds()), "long pause retained hidden rows or exit timer");
    h.hover(0); h.step(.03f); h.settle();
    h.hover(2); h.step(.02f); // Retiring the first branch starts its finite timer.
    h.hover(3); h.step(.02f); h.step(.5f);
    require(h.element("level.1.label.0") && h.element("level.1.label.0")->text == "Other child" &&
            !h.element("level.1.item.0")->disabled, "old exit timer removed a newly opened branch");

    h.open = false; h.step(.02f);
    const float exitOpacity = h.panel().opacity;
    require(exitOpacity > 0 && exitOpacity < 1 && h.element("level.0.item.1")->disabled,
            "root exit must keep content but disable input");
    h.open = true; h.step(.02f);
    require(h.panel().opacity > exitOpacity && h.panel().opacity < 1, "rapid reopen failed to reverse from current opacity");
    require(!h.element("level.1.label.0"), "rapid reopen exposed the old child branch");
    h.animations = false; h.step(0);
    require(core::closeEnough(h.panel().opacity, 1) && !h.runtime.isAnimating(), "animation switch failed to snap endpoint");
    h.animations = true; h.open = false; h.replacement = true; h.step(0);
    require(!h.runtime.isAnimating() && h.element("level.0")->opacity == 0,
            "replacement popup left the old menu animating");
    h.settle();
    h.step(1.0f / 60.0f, false);
    require(!h.runtime.isAnimating() && !h.runtime.paintRequested() &&
            std::isinf(h.runtime.nextTimerWakeSeconds()), "settled menu keeps the event loop awake");
}

void actualMenuIsLimitedAtHighRefresh() {
    for(const int hz : {240,1000}) {
        Harness h; h.step(0,false); h.open=true;
        for(int i=0;i<hz/2;++i) h.step(1.0f/hz,false);
        h.hover(1); h.step(0,false); h.settle();
        int paints=0,fast=0;
        for(int i=0;i<hz;++i) {
            if(i==0 || (i*16/hz)!=((i-1)*16/hz)) h.hover(1+(i*16/hz)%2);
            h.step(1.0f/hz,false); paints+=h.lastPaint?1:0; fast+=h.runtime.isAnimating()?1:0;
        }
        require(paints>10 && paints<=61 && fast==0,"production menu motion exceeded the shared 60 Hz budget");
        h.settle(); h.step(.05f,false);
        require(!h.lastPaint && std::isinf(h.runtime.nextTimerWakeSeconds()),"menu kept waking after convergence");
    }
}

void pointerDispatch() {
    Harness h;
    h.step(0); h.open = true; h.step(.03f); h.settle();
    const auto frame = h.element("level.0.item.1")->frame;
    const double x = frame.x + 30, y = frame.y + frame.height / 2;
    core::queuePointerMotion(h.window, x, y, {}, {}); h.step(0);
    core::queuePointerButton(h.window, x, y, core::PointerButton::Left, core::PointerAction::Press, {}); h.step(0);
    core::queuePointerButton(h.window, x, y, core::PointerButton::Left, core::PointerAction::Release, {}); h.step(0);
    require(h.dispatches == 1 && h.selected == std::vector<int>{1} && !h.open,
            "settled menu hit testing changed command dispatch");
}

} // namespace

int main() {
    try { lifecycle(); actualMenuIsLimitedAtHighRefresh(); pointerDispatch(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    std::cout << "popup entry, single highlight, exit, reversal, input and idle contracts passed\n";
}
