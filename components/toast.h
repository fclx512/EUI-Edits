#pragma once

#include "components/theme.h"
#include "components/text_wrap.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>

namespace components {

struct ToastStyle {
    ToastStyle() : ToastStyle(theme::dark()) {}

    explicit ToastStyle(const theme::ThemeColorTokens& tokens) {
        background = tokens.dark
            ? core::mixColor(tokens.surface, theme::color(0.0f, 0.0f, 0.0f), 0.18f)
            : tokens.surface;
        border = theme::withOpacity(tokens.border, 0.82f);
        text = tokens.text;
        mutedText = theme::withOpacity(tokens.text, 0.68f);
        accent = tokens.primary;
        shadow = theme::popupShadow(tokens);
        radius = tokens.metrics.radius.elevated;
    }

    core::Color background;
    core::Color border;
    core::Color text;
    core::Color mutedText;
    core::Color accent;
    core::Shadow shadow;
    float radius = 14.0f;
};

class ToastBuilder {
public:
    ToastBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    ToastBuilder& visible(bool value = true) { visible_ = value; return *this; }
    ToastBuilder& bindVisible(eui::Signal<bool>& signal) {
        visible(signal.get());
        onDismiss([&signal] { signal.set(false); });
        onAutoDismiss([&signal] { signal.set(false); });
        return *this;
    }
    ToastBuilder& screen(float width, float height) { screenWidth_ = width; screenHeight_ = height; return *this; }
    ToastBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    ToastBuilder& title(const std::string& value) { title_ = value; return *this; }
    ToastBuilder& message(const std::string& value) { message_ = value; return *this; }
    ToastBuilder& icon(unsigned int codepoint) { icon_ = core::dsl::utf8(codepoint); return *this; }
    ToastBuilder& icon(const std::string& value) { icon_ = value; return *this; }
    using IconRenderer = std::function<void(core::dsl::Ui&, const std::string&, float, float, float, core::Color)>;
    ToastBuilder& iconRenderer(IconRenderer value) { iconRenderer_ = std::move(value); return *this; }
    ToastBuilder& fontFamily(const std::string& value) { fontFamily_ = value; return *this; }
    ToastBuilder& fontSize(float value) { fontSize_ = std::round(value); return *this; }
    ToastBuilder& titleFontSize(float value) { titleFontSize_ = std::round(value); return *this; }
    ToastBuilder& style(const ToastStyle& value) { style_ = value; return *this; }
    ToastBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = ToastStyle(tokens);
        metrics_ = tokens.metrics;
        return *this;
    }
    ToastBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    ToastBuilder& zIndex(int value) { zIndex_ = value; return *this; }
    ToastBuilder& duration(float seconds) { autoDismissSeconds_ = std::max(0.0f, seconds); return *this; }
    ToastBuilder& onAutoDismiss(std::function<void()> callback) { onAutoDismiss_ = std::move(callback); return *this; }
    ToastBuilder& onDismiss(std::function<void()> callback) { onDismiss_ = std::move(callback); return *this; }

    void build() {
        struct Lifetime {
            bool visible = false;
            std::string title;
            std::string message;
            std::chrono::steady_clock::time_point deadline;
        };
        auto& lifetime = ui_.state<Lifetime>(id_ + ".lifetime");
        if (visible_ && (!lifetime.visible || lifetime.title != title_ || lifetime.message != message_)) {
            lifetime.deadline = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(autoDismissSeconds_));
        }
        lifetime.visible = visible_;
        lifetime.title = title_;
        lifetime.message = message_;
        const auto deadline = lifetime.deadline;
        if (!visible_ && !transition_.enabled) return;
        const float edgeInset = 16;
        const float width = std::min(width_, std::max(0.0f, screenWidth_-edgeInset*2));
        const float pad = width < 240 ? 10.0f : 16.0f;
        const float closeSize = 28;
        const float iconSize = width < 240 ? 18.0f : 24.0f;
        const float textX = pad+iconSize+12;
        const float textWidth = std::max(1.0f, width-textX-closeSize-pad-4);
        const float font = fontSize_ > 0 ? fontSize_ : std::round(metrics_.typography.label);
        const float titleFont = titleFontSize_ > 0 ? titleFontSize_ : font+3;
        const float lineHeight = std::round(font*1.5f);
        const float titleLineHeight = std::round(titleFont*1.4f);
        const auto bodyLines = text_wrap::lines(message_, textWidth, [&](const std::string& text) {
            return core::TextPrimitive::measureTextWidth(text, fontFamily_, font);
        });
        const auto titleLines = text_wrap::lines(title_, textWidth, [&](const std::string& text) {
            return core::TextPrimitive::measureTextWidth(text, fontFamily_, titleFont, 600);
        });
        const float titleHeight = titleLineHeight*titleLines.size();
        const float messageTop = pad+titleHeight+6;
        const float messageHeight = message_.empty() ? 0 : lineHeight*bodyLines.size();
        const float height = std::min(std::max(height_, messageTop+messageHeight+pad),
                                      std::max(0.0f, screenHeight_-edgeInset*2));
        const float bodyViewport = std::max(0.0f, height-messageTop-pad);
        const float x = std::max(edgeInset, screenWidth_-width-edgeInset);
        const float y = std::max(edgeInset, screenHeight_-height-edgeInset);
        const float visible = visible_ ? 1.0f : 0.0f;
        const float toastOffsetX = visible_ ? 0.0f : 18.0f;
        const float toastOffsetY = visible_ ? 0.0f : 10.0f;
        const std::function<void()> onDismiss = onDismiss_;
        const std::function<void()> onAutoDismiss = onAutoDismiss_ ? onAutoDismiss_ : onDismiss_;

        ui_.stack(id_)
            .x(x)
            .y(y)
            .size(width, height)
            .disabled(!visible_)
            .zIndex(zIndex_)
            .opacity(visible)
            .translate(toastOffsetX, toastOffsetY)
            .transition(transition_)
            .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform)
            .content([&] {
                ui_.rect(id_ + ".bg")
                    .size(width, height)
                    .color(style_.background)
                    .radius(style_.radius)
                    .border(metrics_.spacing.hairline, style_.border)
                    .shadow(style_.shadow)
                    .build();

                if (iconRenderer_) iconRenderer_(ui_, id_+".icon", pad, pad+2, iconSize, style_.accent);
                else ui_.text(id_+".icon").position(pad, pad).size(iconSize, iconSize)
                    .icon(icon_).fontSize(iconSize).color(style_.accent).build();
                ui_.text(id_+".title").position(textX, pad).size(textWidth, titleHeight)
                    .text(text_wrap::join(titleLines)).fontFamily(fontFamily_).fontSize(titleFont).fontWeight(600)
                    .lineHeight(titleLineHeight).color(style_.text).build();
                const std::string scrollId = id_+".body.scroll";
                ui_.stack(scrollId).position(textX, messageTop).size(textWidth, bodyViewport).clip()
                    .scrollState(scrollId, 0, std::max(0.0f, messageHeight-bodyViewport), lineHeight*2)
                    .content([&] {
                        ui_.text(id_+".message").size(textWidth, messageHeight).scrollContentFrom(scrollId)
                            .text(text_wrap::join(bodyLines)).fontFamily(fontFamily_).fontSize(font)
                            .lineHeight(lineHeight).color(style_.mutedText).build();
                    }).build();

                ui_.rect(id_ + ".close.hit")
                    .x(std::max(0.0f, width-closeSize-pad))
                    .y(pad)
                    .size(closeSize, closeSize)
                    .states(theme::color(0.0f, 0.0f, 0.0f, 0.0f),
                            theme::withOpacity(style_.border, 0.36f),
                            theme::withOpacity(style_.border, 0.56f))
                    .radius(metrics_.radius.control)
                    .disabled(!visible_)
                    .onClick(onDismiss)
                    .build();

                const float cx = width-closeSize-pad;
                ui_.polygon(id_+".close.a").position(cx+8, pad+8).size(12,12)
                    .points({{0,1.4f},{1.4f,0},{12,10.6f},{10.6f,12}}).color(style_.mutedText).build();
                ui_.polygon(id_+".close.b").position(cx+8, pad+8).size(12,12)
                    .points({{10.6f,0},{12,1.4f},{1.4f,12},{0,10.6f}}).color(style_.mutedText).build();

                {
                    auto timer = ui_.stack(id_ + ".timer")
                        .size(0.0f, 0.0f);
                    if (visible_ && autoDismissSeconds_ > 0.0f && onAutoDismiss) {
                        // The next frame delta can include time spent inside a
                        // native dialog before this toast appeared. Never let
                        // that historical time dismiss newly visible feedback.
                        timer.onTimer(autoDismissSeconds_, [deadline, onAutoDismiss] {
                            if (std::chrono::steady_clock::now() >= deadline) onAutoDismiss();
                        });
                    }
                    timer.build();
                }
            })
            .build();
    }

private:
    core::dsl::Ui& ui_;
    std::string id_;
    ToastStyle style_;
    theme::ThemeMetricTokens metrics_;
    core::Transition transition_ = core::Transition::make(0.16f, core::Ease::OutCubic);
    std::function<void()> onDismiss_;
    std::function<void()> onAutoDismiss_;
    IconRenderer iconRenderer_;
    std::string fontFamily_;
    float fontSize_ = 0;
    float titleFontSize_ = 0;
    std::string title_ = "Toast";
    std::string message_ = "Short status message for non-blocking feedback.";
    std::string icon_ = core::dsl::utf8(0xF058);
    bool visible_ = false;
    float screenWidth_ = 800.0f;
    float screenHeight_ = 600.0f;
    float width_ = 400.0f;
    float height_ = 88.0f;
    float autoDismissSeconds_ = 0.0f;
    int zIndex_ = 1100;
};

inline ToastBuilder toast(core::dsl::Ui& ui, const std::string& id) {
    return ToastBuilder(ui, id);
}

} // namespace components
