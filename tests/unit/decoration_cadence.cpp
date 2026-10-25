#include "core/dsl_runtime.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (!command.shadowPass) commands.push_back(command);
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}

    std::vector<core::render::RoundedRectDrawCommand> commands;
};

struct Harness {
    int windowTag = 0;
    core::window::Handle window = reinterpret_cast<core::window::Handle>(&windowTag);
    Recorder backend;
    core::render::ScopedRenderBackend scoped{backend};
    core::dsl::Runtime runtime;

    bool start() { return runtime.initialize(); }

    void finish() {
        runtime.shutdown();
        core::releaseInputQueue(window);
    }

    const core::render::RoundedRectDrawCommand* render(float dt) {
        const bool paint = runtime.update(window, dt, 1.0f, 1.0f);
        if (paint) {
            backend.commands.clear();
            runtime.render(160, 100, 1.0f, {0, 0, 0, 1});
            if (!backend.commands.empty()) {
                lastCommand = backend.commands.back();
                hasLastCommand = true;
            }
        }
        return hasLastCommand ? &lastCommand : nullptr;
    }

    core::render::RoundedRectDrawCommand lastCommand;
    bool hasLastCommand = false;
};

bool close(float a, float b, float epsilon = 0.0005f) {
    return std::fabs(a - b) <= epsilon;
}

bool changedDecoration(const core::render::RoundedRectDrawCommand& a,
                       const core::render::RoundedRectDrawCommand& b) {
    return !close(a.fillColor.r, b.fillColor.r) || !close(a.fillColor.g, b.fillColor.g) ||
           !close(a.fillColor.b, b.fillColor.b) || !close(a.fillColor.a, b.fillColor.a) ||
           !close(a.opacity, b.opacity) || !close(a.border.width, b.border.width) ||
           !close(a.border.color.r, b.border.color.r) || !close(a.border.color.g, b.border.color.g) ||
           !close(a.border.color.b, b.border.color.b) || !close(a.border.color.a, b.border.color.a) ||
           !close(a.shadowBlur, b.shadowBlur) || !close(a.shadowOffset.x, b.shadowOffset.x) ||
           !close(a.shadowOffset.y, b.shadowOffset.y) || !close(a.rect.x, b.rect.x) ||
           !close(a.rect.y, b.rect.y) || !close(a.rect.width, b.rect.width) ||
           !close(a.rect.height, b.rect.height) || !close(a.radius, b.radius) ||
           !close(a.backdropBlur, b.backdropBlur);
}

enum class Field { Color, Opacity };

bool highRateFieldIsSampled(Field field, int hz) {
    Harness h;
    if (!h.start()) return false;
    h.runtime.setDecorationFrameRateLimit(60.0f);
    float current = field == Field::Color ? 0.1f : 0.15f;
    auto compose = [&] {
        h.runtime.compose("decoration-cadence-field", 160, 100,
            [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                auto rect = ui.rect("sample").position(20, 20).size(80, 40);
                if (field == Field::Color) rect.color({current, 0.25f, 0.4f, 1.0f});
                else rect.color({0.2f, 0.25f, 0.4f, 1.0f}).opacity(current);
                rect.transition(core::Transition::make(0.5f, core::Ease::Linear)
                                    .animate(field == Field::Color ? core::AnimProperty::Color
                                                                  : core::AnimProperty::Opacity))
                    .build();
            });
    };

    compose();
    const auto* initialPtr = h.render(0.0f);
    if (!initialPtr) { h.finish(); return false; }
    auto previous = *initialPtr;
    int changes = 0;
    for (int i = 0; i < hz; ++i) {
        const int togglePeriod = std::max(1, hz / 15);
        current = ((i / togglePeriod) & 1) ? (field == Field::Color ? 0.1f : 0.15f)
                                           : (field == Field::Color ? 0.9f : 0.95f);
        compose();
        const auto* frame = h.render(1.0f / static_cast<float>(hz));
        if (frame && changedDecoration(previous, *frame)) ++changes;
        if (frame) previous = *frame;
    }
    h.finish();
    if (changes < 10 || changes > 61) {
        std::cerr << "FAIL: " << (field == Field::Color ? "color" : "opacity")
                  << " changed " << changes << " times at " << hz << " updates/sec\n";
        return false;
    }
    return true;
}

bool decorationPropertiesShareOneSampleCadence() {
    Harness h;
    if (!h.start()) return false;
    h.runtime.setDecorationFrameRateLimit(60.0f);
    float value = 0.1f;
    auto compose = [&] {
        h.runtime.compose("decoration-cadence-shared-fields", 160, 100,
            [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                ui.rect("target").position(10, 10).size(100, 60)
                    .color({value, 0.25f, 0.4f, 1})
                    .opacity(0.2f + value * 0.7f)
                    .border(1.0f + value * 2.0f, {value, 0.8f, 0.3f, 1})
                    .shadow(2.0f + value * 8.0f, 1.0f, 2.0f, {0.1f, 0.1f, 0.1f, value})
                    .states({0.1f, 0.25f, 0.4f, 1}, {0.5f, 0.3f, 0.2f, 1},
                            {0.9f, 0.2f, 0.1f, 1})
                    .color({value, 0.25f, 0.4f, 1})
                    .transition(core::Transition::make(0.5f, core::Ease::Linear)
                                    .animate(core::AnimProperty::Color | core::AnimProperty::Opacity |
                                             core::AnimProperty::Border | core::AnimProperty::Shadow))
                    .onClick([] {})
                    .build();
            });
    };
    compose();
    const auto* initial = h.render(0.0f);
    if (!initial) { h.finish(); return false; }
    auto previous = *initial;
    core::queuePointerMotion(h.window, 20, 20, {}, {});
    core::queuePointerButton(h.window, 20, 20, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);

    int changes = 0;
    constexpr int hz = 240;
    constexpr int togglePeriod = hz / 15;
    for (int i = 0; i < hz; ++i) {
        value = ((i / togglePeriod) & 1) ? 0.1f : 0.9f;
        compose();
        const auto* frame = h.render(1.0f / static_cast<float>(hz));
        if (frame && changedDecoration(previous, *frame)) ++changes;
        if (frame) previous = *frame;
    }
    core::queuePointerButton(h.window, 20, 20, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);
    h.finish();
    if (changes > 61 || changes < 10) {
        std::cerr << "FAIL: color/opacity/border/shadow/hover/press exceeded or missed the shared cadence ("
                  << changes << " observed changes)\n";
        return false;
    }
    return true;
}

bool timerWakeAndInputStayLive() {
    Harness h;
    if (!h.start()) return false;
    h.runtime.setDecorationFrameRateLimit(60.0f);
    int moves = 0, presses = 0, releases = 0, drags = 0, scrolls = 0, timerCalls = 0;
    float opacity = 0.2f;
    h.runtime.compose("decoration-cadence-live-input", 160, 100,
        [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            ui.rect("target").position(10, 10).size(100, 60)
                .color({0.2f, 0.3f, 0.4f, 1}).opacity(opacity)
                .transition(core::Transition::make(0.5f).animate(core::AnimProperty::Opacity))
                .onMove([&](const core::PointerEvent&, const core::Rect&) { ++moves; return false; })
                .onPress([&](const core::PointerEvent&, const core::Rect&) { ++presses; })
                .onRelease([&](const core::PointerEvent&, const core::Rect&) { ++releases; })
                .onDrag([&](const core::dsl::DragEvent&) { ++drags; })
                .onScroll([&](const core::ScrollEvent&) { ++scrolls; })
                .onTimer(0.2f, [&] { ++timerCalls; }).build();
        });
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);

    opacity = 0.95f;
    h.runtime.compose("decoration-cadence-live-input", 160, 100,
        [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
            ui.rect("target").position(10, 10).size(100, 60)
                .color({0.2f, 0.3f, 0.4f, 1}).opacity(opacity)
                .transition(core::Transition::make(0.5f).animate(core::AnimProperty::Opacity))
                .onMove([&](const core::PointerEvent&, const core::Rect&) { ++moves; return false; })
                .onPress([&](const core::PointerEvent&, const core::Rect&) { ++presses; })
                .onRelease([&](const core::PointerEvent&, const core::Rect&) { ++releases; })
                .onDrag([&](const core::dsl::DragEvent&) { ++drags; })
                .onScroll([&](const core::ScrollEvent&) { ++scrolls; })
                .onTimer(0.2f, [&] { ++timerCalls; }).build();
        });
    h.runtime.update(h.window, 0.001f, 1.0f, 1.0f);
    const bool decorationSleeps = !h.runtime.isAnimating() &&
        std::isfinite(h.runtime.nextTimerWakeSeconds()) &&
        h.runtime.nextTimerWakeSeconds() > 0.0f;

    core::queuePointerMotion(h.window, 20, 20, {}, {});
    core::queuePointerButton(h.window, 20, 20, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerMotion(h.window, 30, 25, core::PointerButton::Left, {});
    core::queueScrollInput(h.window, 0, -1);
    core::queuePointerButton(h.window, 30, 25, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);
    const bool inputLive = moves >= 2 && presses == 1 && releases == 1 &&
                           drags >= 1 && scrolls == 1;

    h.runtime.update(h.window, 0.20f, 1.0f, 1.0f);
    const bool timerLive = timerCalls == 1 && std::isfinite(h.runtime.nextTimerWakeSeconds());
    h.finish();
    if (!decorationSleeps || !inputLive || !timerLive) {
        std::cerr << "FAIL: decoration sampling blocked its timer wake or pointer/scroll callbacks\n";
        return false;
    }
    return true;
}

bool decorationWakeIsScheduledWithoutContinuousAnimation() {
    Harness h;
    if (!h.start()) return false;
    h.runtime.setDecorationFrameRateLimit(60.0f);
    float opacity = 0.2f;
    auto compose = [&] {
        h.runtime.compose("decoration-cadence-wake", 160, 100,
            [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                ui.rect("target").position(10, 10).size(100, 60)
                    .color({0.2f, 0.3f, 0.4f, 1}).opacity(opacity)
                    .transition(core::Transition::make(0.5f).animate(core::AnimProperty::Opacity))
                    .build();
            });
    };
    compose();
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);
    opacity = 0.95f;
    compose();
    h.runtime.update(h.window, 0.001f, 1.0f, 1.0f);
    const bool scheduled = !h.runtime.isAnimating() &&
        std::isfinite(h.runtime.nextTimerWakeSeconds()) &&
        h.runtime.nextTimerWakeSeconds() > 0.0f;
    h.finish();
    if (!scheduled) {
        std::cerr << "FAIL: decoration animation kept continuous animation active instead of scheduling a wake\n";
    }
    return scheduled;
}

bool geometryTransitionsAreSampledAndDirectLayoutStaysLive() {
    for (const bool animated : {true, false}) {
        Harness h;
        if (!h.start()) return false;
        float x = 10;
        const auto compose = [&] {
            h.runtime.compose("geometry-cadence", 160, 100, [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                auto target = ui.rect("target").position(x,10).size(40,30).color({.2f,.3f,.4f,1});
                if (animated) target.transition(core::Transition::make(.5f,core::Ease::Linear))
                    .animate(core::AnimProperty::Frame | core::AnimProperty::Radius | core::AnimProperty::Transform | core::AnimProperty::Blur)
                    .radius(x*.1f).translateX(x*.2f).blur(x*.01f);
                target.build();
            });
        };
        compose(); const auto* initial=h.render(0);
        if(!initial) { h.finish(); return false; }
        auto previous=*initial; int changes=0,fast=0;
        for(int i=0;i<240;++i) {
            x=(i&1)?10.0f:100.0f; compose();
            const auto* frame=h.render(1.0f/240);
            if(frame && changedDecoration(previous,*frame)) ++changes;
            if(frame) previous=*frame;
            fast+=h.runtime.isAnimating()?1:0;
        }
        h.finish();
        if((animated && (changes<10 || changes>61 || fast)) || (!animated && changes<180)) {
            std::cerr<<"FAIL: passive geometry/direct layout contract animated="<<animated<<" changes="<<changes<<" fast="<<fast<<"\n";
            return false;
        }
    }
    return true;
}

bool zeroDeltaCompositeEventsDoNotAdvanceAndAnimationSettles() {
    Harness h;
    if (!h.start()) return false;
    h.runtime.setDecorationFrameRateLimit(60.0f);
    int pressCount = 0, releaseCount = 0;
    float color = 0.1f;
    auto compose = [&] {
        h.runtime.compose("decoration-cadence-zero-delta", 160, 100,
            [&](core::dsl::Ui& ui, const core::dsl::Screen&) {
                ui.rect("target").position(10, 10).size(100, 60)
                    .color({color, 0.3f, 0.4f, 1})
                    .transition(core::Transition::make(0.2f, core::Ease::Linear)
                                    .animate(core::AnimProperty::Color))
                    .onPress([&](const core::PointerEvent&, const core::Rect&) { ++pressCount; })
                    .onRelease([&](const core::PointerEvent&, const core::Rect&) { ++releaseCount; })
                    .build();
            });
    };
    compose();
    const auto* first = h.render(0.0f);
    if (!first) { h.finish(); return false; }
    const float startColor = first->fillColor.r;

    color = 0.9f;
    compose();
    h.render(0.0f);
    core::queuePointerButton(h.window, 20, 20, core::PointerButton::Left,
                             core::PointerAction::Press, {});
    core::queuePointerButton(h.window, 20, 20, core::PointerButton::Left,
                             core::PointerAction::Release, {});
    h.runtime.update(h.window, 0.0f, 1.0f, 1.0f);
    const bool callbacks = pressCount == 1 && releaseCount == 1;
    const float zeroDeltaColor = [&] {
        const auto* frame = h.render(0.0f);
        return frame ? frame->fillColor.r : std::numeric_limits<float>::quiet_NaN();
    }();
    bool stableAtZero = close(zeroDeltaColor, startColor);
    for (int i = 0; i < 8; ++i) {
        const auto* frame = h.render(0.0f);
        if (!frame || !close(frame->fillColor.r, startColor)) {
            stableAtZero = false;
        }
    }

    h.render(0.05f);
    color = 0.1f; // Reverse the in-flight target; the cancelled path must settle at the new endpoint.
    compose();
    h.render(0.0f); // Install the new target before its elapsed interval.
    const auto* settledFrame = h.render(0.30f);
    const bool settled = settledFrame && close(settledFrame->fillColor.r, startColor) &&
        !h.runtime.isAnimating() && std::isinf(h.runtime.nextTimerWakeSeconds());
    h.finish();
    if (!callbacks || !stableAtZero || !settled) {
        std::cerr << "FAIL: zero-delta events advanced decoration or the transition failed to settle\n";
        return false;
    }
    return true;
}

bool newTargetAfterStallAndDisable() {
    Harness h;
    if (!h.start()) return false;
    float first = .1f, second = .1f;
    bool animate = true;
    const auto compose = [&] {
        h.runtime.compose("stall-disable",160,100,[&](core::dsl::Ui& ui,const core::dsl::Screen&) {
            ui.rect("older").position(5,5).size(30,30).color({first,.2f,.3f,1})
                .transition(core::Transition::make(.5f).animate(core::AnimProperty::Color)).build();
            ui.rect("newer").position(70,20).size(40,40).color({second,.2f,.3f,1})
                .transition(animate ? core::Transition::make(.5f).animate(core::AnimProperty::Color)
                                    : core::Transition::none()).build();
        });
    };
    compose(); h.render(0);
    first = .9f; compose(); h.render(.02f);
    second = .9f; compose();
    const auto* afterStall = h.render(2.0f);
    const bool started = afterStall && afterStall->fillColor.r < .5f &&
        std::isfinite(h.runtime.nextTimerWakeSeconds());
    animate = false; compose();
    const auto* disabled = h.render(0);
    const bool snapped = disabled && close(disabled->fillColor.r,.9f) &&
        !h.runtime.isAnimating() && std::isinf(h.runtime.nextTimerWakeSeconds());
    h.finish();
    if (!started || !snapped) std::cerr << "FAIL: new target ate pre-activation stall or animation-off failed to snap\n";
    return started && snapped;
}

bool targetBetweenSamplesDoesNotConsumeEarlierTime() {
    Harness h;
    if (!h.start()) return false;
    float first = .1f, second = .1f;
    const auto compose = [&] {
        h.runtime.compose("between-samples",160,100,[&](core::dsl::Ui& ui,const core::dsl::Screen&) {
            ui.rect("older").position(5,5).size(30,30).color({first,.2f,.3f,1})
                .transition(core::Transition::make(.5f,core::Ease::Linear).animate(core::AnimProperty::Color)).build();
            ui.rect("newer").position(70,20).size(40,40).color({second,.2f,.3f,1})
                .transition(core::Transition::make(.5f,core::Ease::Linear).animate(core::AnimProperty::Color)).build();
        });
    };
    compose(); h.render(0);
    first = .9f; compose(); h.render(.02f);
    h.render(.012f); // Most of this shared interval predates the second target.
    second = .9f; compose(); h.render(0);
    const auto* sampled = h.render(.005f);
    const bool correctAge = sampled && close(sampled->fillColor.r,.108f,.002f);
    const auto* settled = h.render(.6f);
    const bool caughtUp = settled && close(settled->fillColor.r,.9f);
    h.finish();
    if (!correctAge || !caughtUp) std::cerr << "FAIL: new target consumed time before activation or failed to catch up\n";
    return correctAge && caughtUp;
}

} // namespace

int main() {
    bool ok = newTargetAfterStallAndDisable();
    ok = targetBetweenSamplesDoesNotConsumeEarlierTime() && ok;
    for (int hz : {240, 1000}) {
        ok = highRateFieldIsSampled(Field::Color, hz) && ok;
        ok = highRateFieldIsSampled(Field::Opacity, hz) && ok;
    }
    ok = decorationPropertiesShareOneSampleCadence() && ok;
    ok = decorationWakeIsScheduledWithoutContinuousAnimation() && ok;
    ok = geometryTransitionsAreSampledAndDirectLayoutStaysLive() && ok;
    ok = timerWakeAndInputStayLive() && ok;
    ok = zeroDeltaCompositeEventsDoNotAdvanceAndAnimationSettles() && ok;
    std::cout << (ok ? "PASS" : "FAIL") << ": decoration_cadence\n";
    return ok ? 0 : 1;
}
