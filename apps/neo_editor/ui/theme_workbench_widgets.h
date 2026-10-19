#pragma once

// 工作台内部的小控件：尺寸一律由 uiMetrics 派生，配色取应用当前外观（chrome），
// 与设置页的 flatButton / segmentedAt 同一套视觉语言。
#include "components/segmented.h"
#include "components/switch.h"
#include "ui/icons.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace neo::theme_workbench_ui {

struct Dim {
    float font = 14, hint = 13, title = 17, control = 32, row = 40, pad = 20, gap = 8;
};
inline Dim dims(const AppState& state) {
    const UiMetrics m = uiMetrics(state);
    Dim d;
    d.font = m.panelFontSize;
    d.hint = m.panelHintFontSize;
    d.title = m.panelLabelFontSize;
    d.control = std::max(30.0f, std::round(d.font * 2.3f));
    d.row = std::max(34.0f, std::round(d.font * 2.85f));
    d.pad = std::max(16.0f, std::round(d.font * 1.45f));
    d.gap = std::max(6.0f, std::round(d.font * .57f));
    return d;
}

inline eui::Color accentTint(const EditorColors& c, float alpha) {
    return rgba(c.accent.r, c.accent.g, c.accent.b, alpha);
}

inline void label(eui::Ui& ui, const std::string& id, const std::string& value, float x, float y,
                  float w, float h, eui::Color color, float size, int weight = 400,
                  eui::HorizontalAlign align = eui::HorizontalAlign::Left) {
    ui.text(id).position(x, y).size(std::max(1.0f, w), std::max(1.0f, h))
        .text(elideToWidth(value, w, size)).fontSize(size).fontWeight(weight).color(color)
        .horizontalAlign(align).verticalAlign(eui::VerticalAlign::Center).build();
}

inline void paragraph(eui::Ui& ui, const std::string& id, const std::string& value, float x, float y,
                      float w, float h, eui::Color color, float size) {
    ui.text(id).position(x, y).size(std::max(1.0f, w), std::max(1.0f, h)).text(value)
        .fontSize(size).color(color).wrap(true).lineHeight(std::round(size * 1.5f))
        .verticalAlign(eui::VerticalAlign::Top).build();
}

// 无底色按钮：悬停/按下给反馈；active 为强调色浅底。
inline void ghostButton(eui::Ui& ui, const std::string& id, const std::string& text, float x, float y,
                        float w, float h, const EditorColors& c, float size, std::function<void()> click,
                        bool active = false, bool disabled = false, UiIcon icon = UiIcon::None) {
    const eui::Color fg = disabled ? core::mixColor(c.textMuted, c.toolbar, .45f) : active ? c.accent : c.text;
    ui.stack(id).position(x, y).size(std::max(1.0f, w), h).content([&] {
        auto bg = ui.rect(id + ".bg").fill().radius(6.0f)
            .states(active ? accentTint(c, .14f) : transparentColor(),
                    active ? accentTint(c, .20f) : c.rowHover, active ? accentTint(c, .26f) : c.pressed)
            .transition(quickTransition()).preserveFocusOnPress().disabled(disabled);
        if (!disabled) bg.cursor(eui::CursorShape::Hand).onClick(std::move(click));
        bg.build();
        const float glyph = std::round(size * 1.05f);
        const bool hasIcon = icon != UiIcon::None;
        const float textW = estimateTextWidth(text, size);
        const float total = (hasIcon ? glyph + (text.empty() ? 0 : 6) : 0) + textW;
        const float start = std::max(4.0f, (w - total) * .5f);
        if (hasIcon) iconView(ui, id + ".icon", icon, start, (h - glyph) * .5f, glyph, fg);
        if (!text.empty())
            label(ui, id + ".text", text, hasIcon ? start + glyph + 6 : 0, 0, hasIcon ? w - start - glyph - 6 : w,
                  h, fg, size, 400, hasIcon ? eui::HorizontalAlign::Left : eui::HorizontalAlign::Center);
    }).build();
}

inline void primaryButton(eui::Ui& ui, const std::string& id, const std::string& text, float x, float y,
                          float w, float h, const EditorColors& c, float size, std::function<void()> click,
                          bool disabled = false) {
    const eui::Color base = disabled ? core::mixColor(c.accent, c.toolbar, .55f) : c.accent;
    ui.stack(id).position(x, y).size(std::max(1.0f, w), h).content([&] {
        auto bg = ui.rect(id + ".bg").fill().radius(6.0f)
            .states(base, core::mixColor(base, rgba(1, 1, 1), .10f), core::mixColor(base, rgba(0, 0, 0), .12f))
            .transition(quickTransition()).preserveFocusOnPress().disabled(disabled);
        if (!disabled) bg.cursor(eui::CursorShape::Hand).onClick(std::move(click));
        bg.build();
        const float l = .2126f * c.accent.r + .7152f * c.accent.g + .0722f * c.accent.b;
        label(ui, id + ".text", text, 0, 0, w, h, l > .62f ? rgba(.08f, .09f, .11f) : rgba(1, 1, 1),
              size, 600, eui::HorizontalAlign::Center);
    }).build();
}

inline void segmented(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                      const EditorColors& c, float size, const std::vector<std::string>& items, int selected,
                      std::function<void(int)> change) {
    ui.stack(id + ".slot").position(x, y).size(w, h).content([&] {
        components::segmented(ui, id).size(w, h).items(items).selected(selected).fontSize(size)
            .theme(c.tokens).transition(quickTransition()).onChange(std::move(change)).build();
    }).build();
}

inline void toggle(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                   const EditorColors& c, float size, const std::string& text, bool checked,
                   std::function<void(bool)> change) {
    ui.stack(id + ".slot").position(x, y).size(w, h).content([&] {
        components::toggleSwitch(ui, id).size(w, h).text(text).checked(checked).fontSize(size)
            .trackSize(std::round(size * 2.4f), std::round(size * 1.3f))
            .theme(c.tokens).transition(quickTransition()).onChange(std::move(change)).build();
    }).build();
}

inline void hairline(eui::Ui& ui, const std::string& id, float x, float y, float w, float h, const EditorColors& c) {
    ui.rect(id).position(x, y).size(std::max(1.0f, w), std::max(1.0f, h))
        .color(rgba(c.border.r, c.border.g, c.border.b, c.border.a * .7f)).build();
}

// 色块：半透明颜色画在两格棋盘上，透明度才看得出来。
inline void swatch(eui::Ui& ui, const std::string& id, float x, float y, float w, float h,
                   eui::Color color, const EditorColors& c, float radius = 5.0f) {
    ui.stack(id).position(x, y).size(w, h).clip().content([&] {
        ui.rect(id + ".a").size(w, h).radius(radius).color(rgba(1, 1, 1)).build();
        if (color.a < .995f)
            for (int i = 0; i < 2; ++i)
                ui.rect(id + ".chk." + std::to_string(i)).position(i ? w * .5f : 0, i ? 0 : h * .5f)
                    .size(w * .5f, h * .5f).color(rgba(.80f, .81f, .83f)).build();
        ui.rect(id + ".fill").size(w, h).radius(radius).color(color)
            .border(1.0f, rgba(c.text.r, c.text.g, c.text.b, .14f)).build();
    }).build();
}

} // namespace neo::theme_workbench_ui
