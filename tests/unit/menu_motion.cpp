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
    std::vector<core::Rect> scissors;
    core::Rect scissor{};
    bool hasScissor = false;
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
    void setScissor(bool enabled, const core::Rect& value, int) override { hasScissor = enabled; scissor = value; }
    void prepareBackdropBlur(const core::Rect&, float, int, int) override { ++materialRequests; }
    void prepareFrozenBackdrop(const core::FrozenBackdrop&) override { ++materialRequests; }
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (!command.shadowPass) {
            commands.push_back(command);
            scissors.push_back(hasScissor ? scissor : core::Rect{});
        }
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
    bool clipped = false;
    float x = 30, height = 480, contentOffsetY = -8;
    int extraRows = 0;
    int dispatches = 0;
    std::vector<int> selected;
    components::theme::ThemeColorTokens tokens = components::theme::light();

    Harness() {
        require(runtime.initialize(), "runtime initialization");
        tokens.surfaceHover = {0.31f, 0.44f, 0.57f, 1};
    }
    ~Harness() { runtime.shutdown(); core::releaseInputQueue(window); }

    void step(float dt, bool forcePaint = true) {
        runtime.compose("menu-motion", 640, height, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
            ui = &view;
            components::ContextMenuMotion motion;
            motion.clipContent = clipped;
            motion.directionalSubmenus = clipped;
            motion.offsetY = clipped ? contentOffsetY : -4;
            if (animations) {
                motion.enter = core::Transition::make(.12f, core::Ease::OutCubic);
                if (!replacement) motion.exit = core::Transition::make(.07f, core::Ease::OutQuad);
                if (!clipped) motion.highlight = core::Transition::make(.075f, core::Ease::OutCubic);
            }
            using Item = components::ContextMenuItem;
            std::vector<Item> items{Item("Parent", {Item("Child A", {Item("Grandchild")}), Item("Child B")}),
                                    Item("Action 1"), Item("Action 2"), Item("Other Parent", {Item("Other child")})};
            for (int i = 0; i < extraRows; ++i) items.emplace_back("Extra " + std::to_string(i));
            components::contextMenu(view, "sample").open(open).screen(640, height).position(x, 50).size(210, 32)
                .items(std::move(items))
                .theme(tokens).motion(motion).skipClosedContent()
                .onSelectPath([&](const std::vector<int>& path) { ++dispatches; selected = path; })
                .onOpenChange([&](bool value) { open = value; }).build();
        });
        require(!ui->frozenBackdropSession() && !ui->cachedStripSession(), "opaque menu requested a cache session");
        lastPaint = runtime.update(window, dt, 1, 1);
        if (forcePaint) { runtime.requestFullPaint(); lastPaint = true; }
        if (lastPaint) {
            backend.commands.clear();
            backend.scissors.clear();
            runtime.render(640, static_cast<int>(height), 1, {1, 1, 1, 1});
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
    void click(double px, double py) {
        core::queuePointerMotion(window, px, py, {}, {}); step(0);
        core::queuePointerButton(window, px, py, core::PointerButton::Left, core::PointerAction::Press, {}); step(0);
        core::queuePointerButton(window, px, py, core::PointerButton::Left, core::PointerAction::Release, {}); step(0);
    }
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

core::Rect drawnBounds(const core::render::RoundedRectDrawCommand& command) {
    float left = std::numeric_limits<float>::infinity(), top = left;
    float right = -left, bottom = -left;
    for (const auto& vertex : command.vertices) {
        left = std::min(left, vertex.screen.x); top = std::min(top, vertex.screen.y);
        right = std::max(right, vertex.screen.x); bottom = std::max(bottom, vertex.screen.y);
    }
    return {left, top, right-left, bottom-top};
}

std::size_t highlightCommand(Harness& h, int depth = 0) {
    const auto* highlight = h.element("level." + std::to_string(depth) + ".highlight");
    require(highlight != nullptr, "highlight element missing");
    for (std::size_t i = 0; i < h.backend.commands.size(); ++i) {
        const auto& command = h.backend.commands[i];
        if (core::closeEnough(command.fillColor, h.tokens.surfaceHover) &&
            core::closeEnough(command.rect.x, highlight->frame.x) &&
            core::closeEnough(command.rect.y, highlight->frame.y)) return i;
    }
    throw std::runtime_error("highlight draw missing");
}

void lifecycle(bool clipped) {
    Harness h;
    h.clipped = clipped;
    h.step(0); // Prime the closed layout instance before its first open.
    h.open = true;
    h.step(.03f);
    const auto entering = h.panel();
    require(entering.opacity > 0 && entering.opacity < 1, "first open did not animate");
    require(core::closeEnough(h.element("level.0")->transform.scale, core::Vec2{1,1}), "text scale changed");
    h.settle();
    require(core::closeEnough(h.panel().opacity, 1), "enter did not reach opaque endpoint");
    require(core::closeEnough(spanX(entering), spanX(h.panel())), "popup width scaled during entry");
    if (clipped) require(core::closeEnough(drawnBounds(entering), drawnBounds(h.panel())),
                         "clipped entry moved or resized the panel/shadow geometry");

    h.hover(1); h.step(0); h.settle();
    const float start = h.highlightY();
    h.hover(2); h.step(1.0f / 240.0f);
    const float first = h.highlightY();
    const float target = h.element("level.0.highlight")->frame.y;
    if (clipped) {
        require(core::closeEnough(first, target), "clipped menu retained sliding hover feedback");
    } else {
    require(first > start && first < target, "adjacent highlight did not interpolate");
    h.step(1.0f / 240.0f);
    require(core::closeEnough(h.highlightY(), first), "highlight advanced between 60 Hz samples");
    for (int i=0;i<3;++i) h.step(1.0f/240);
    const float second = h.highlightY();
    require(second > first && second < target, "shared sample snapped or stalled the active highlight");
    }
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
    for (const bool clipped : {false, true}) for(const int hz : {240,1000}) {
        Harness h; h.clipped=clipped; h.step(0,false); h.open=true;
        for(int i=0;i<hz/2;++i) h.step(1.0f/hz,false);
        h.hover(1); h.step(0,false); h.settle();
        int paints=0,fast=0;
        for(int i=0;i<hz;++i) {
            if(i==0 || (i*16/hz)!=((i-1)*16/hz)) h.hover(1+(i*16/hz)%2);
            h.step(1.0f/hz,false); paints+=h.lastPaint?1:0; fast+=h.runtime.isAnimating()?1:0;
        }
        // A direct row switch can paint once per changed input target; only
        // interpolated feedback is expected to use the shared sample count.
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

void clippedInputAndSubmenuDirections() {
    // Exaggerate the offset to leave an action partially clipped. Probe both
    // sides of the visible boundary while the entry animation is still active.
    for (const bool visiblePart : {false, true}) {
        Harness h; h.clipped = true; h.contentOffsetY = -64;
        h.step(0); h.open = true; h.step(.01f);
        h.hover(1); h.step(0);
        const auto index = highlightCommand(h);
        const auto bounds = drawnBounds(h.backend.commands[index]);
        const auto clip = h.backend.scissors[index];
        require(bounds.y < clip.y && bounds.y + bounds.height > clip.y,
                "entry input probe did not straddle the fixed clip");
        h.click(bounds.x + bounds.width / 2, visiblePart ? clip.y + 1 : bounds.y + 1);
        require(h.dispatches == (visiblePart ? 1 : 0), "entry hit testing ignored the visible clip");
        if (visiblePart) require(h.selected == std::vector<int>{1}, "entry hit testing selected the untransformed row");
    }

    for (const bool opensRight : {true, false}) {
        Harness h; h.clipped = true; h.x = opensRight ? 30 : 390;
        h.step(0); h.open = true; h.settle();
        h.hover(0); h.step(.01f);
        auto* child = h.element("level.1.item.1");
        require(child && child->onHoverChanged, "directional submenu was not composed");
        child->onHoverChanged(true); h.step(.01f);
        const auto i = highlightCommand(h, 1);
        const auto entering = drawnBounds(h.backend.commands[i]);
        const auto clip = h.backend.scissors[i];
        const float target = h.element("level.1.highlight")->frame.x;
        require(opensRight ? entering.x < target : entering.x > target,
                "first submenu entry did not follow its resolved side");
        const auto* root = h.element("level.0");
        require(opensRight ? clip.x > root->frame.x + root->frame.width : clip.x + clip.width < root->frame.x,
                "parent clip leaked into the sibling submenu");
        h.settle();
        child = h.element("level.1.item.1");
        h.click(child->frame.x + child->frame.width / 2, child->frame.y + child->frame.height / 2);
        require(h.dispatches == 1 && h.selected == std::vector<int>({0,1}),
                "sibling submenu could not dispatch outside the parent clip");
    }

    Harness closed; closed.clipped = true;
    closed.step(0); closed.open = true; closed.settle();
    const auto frame = closed.element("level.0.item.1")->frame;
    closed.open = false; closed.step(.01f);
    closed.click(frame.x + 20, frame.y + frame.height / 2);
    require(closed.dispatches == 0, "retiring clipped content retained command input");
}

void clippedShortMenuScroll() {
    Harness h; h.clipped = true; h.height = 220; h.extraRows = 12;
    h.step(0); h.open = true; h.settle();
    const auto viewportFrame = h.element("level.0.viewport")->frame;
    const auto firstFrame = h.element("level.0.item.0")->frame;
    h.ui->state<float>("sample.level.0.offset") = 64;
    h.step(0); // Scroll follows the live binding, without an animation wait.
    require(core::closeEnough(h.element("level.0.item.0")->frame, firstFrame) &&
            core::closeEnough(h.element("level.0.viewport")->frame, viewportFrame),
            "clipped scroll changed the final layout/viewport geometry");
    const auto action = h.element("level.0.item.2")->frame;
    h.click(action.x + action.width / 2, action.y - 64 + action.height / 2);
    require(h.dispatches == 1 && h.selected == std::vector<int>{2},
            "clipped content animated or ignored the independent scroll offset");
}

} // namespace

int main() {
    try {
        lifecycle(false); lifecycle(true); actualMenuIsLimitedAtHighRefresh(); pointerDispatch();
        clippedInputAndSubmenuDirections(); clippedShortMenuScroll();
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    std::cout << "popup entry, single highlight, exit, reversal, input and idle contracts passed\n";
}
