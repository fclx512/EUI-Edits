#pragma once

#include "eui_neo.h"
#include "state/app_state.h"
#include "ui/icons.h"

#include <string>

namespace neo {

inline eui::Transition quickTransition() {
    return eui::Transition::make(0.14f, eui::Ease::OutCubic);
}

// 动画开关：设置面板的"动画效果"（落盘 settings.ini 的 animations）。
// NEO_FEEDBACK=0 可强制关闭，用于排查和同二进制 A/B 实测开销。
//
// 开启时不改写任何东西 —— 直接用 UI 库自带的交互反馈：Element 默认的
// smoothStateColors 悬停/按压配色平滑，加上控件自己声明的 transition。
// 关闭时由 applyInteractionDefaults 统一压成瞬时切换。
inline bool animationsEnabled(const AppState& state) {
    if (const char* value = std::getenv("NEO_FEEDBACK")) {
        if (value[0] == '0') {
            return false;
        }
    }
    return state.animations;
}

// compose 末尾统一施加：界面字体默认值，以及关闭动画时的瞬时化。
// pressedScale 一并清掉 —— 按压缩放也属于反馈动画。
//
// 关闭不需要单独的"收尾"逻辑：下一帧 element.transition 已是 none、
// shouldAnimate() 返回 false，AnimatedValue::setTarget 会直接把 current_ 落到
// 目标值并清 active_（core/animation.h:293），正在跑的过渡不会冻在半路。
inline void applyInteractionDefaults(eui::Ui& ui, const char* fontFamily, bool animations) {
    const auto visit = [fontFamily, animations](auto&& self, core::dsl::Element& element) -> void {
        if (!animations) {
            element.transition = eui::Transition::none();
            element.smoothStateColors = false;
            element.pressedScale = 1.0f;
        }
        if (element.fontFamily.empty()) element.fontFamily = fontFamily;
        for (const auto& child : element.children) self(self, *child);
    };
    for (const auto& root : ui.roots()) visit(visit, *root);
}

inline eui::Color transparentColor() {
    return rgba(0.0f, 0.0f, 0.0f, 0.0f);
}

// 竖分隔线，只做视觉分组。
inline void dividerView(eui::Ui& ui, const std::string& id, float height) {
    ui.rect(id)
        .size(1.0f, height)
        .color(editorColors().border)
        .build();
}

// 框架没有省略号截断，也没有文本测量接口给应用用，长文件名只能自己按估算宽度裁。
// 估算规则：CJK 记 1 字宽，拉丁记 0.55，两字节区记 0.62。
inline std::size_t glyphLength(unsigned char leadByte) {
    if (leadByte >= 0xF0u) {
        return 4;
    }
    if (leadByte >= 0xE0u) {
        return 3;
    }
    if (leadByte >= 0xC0u) {
        return 2;
    }
    return 1;
}

inline float glyphAdvance(unsigned char leadByte, float fontSize) {
    if (leadByte >= 0xE0u) {
        return fontSize;
    }
    if (leadByte >= 0xC0u) {
        return fontSize * 0.62f;
    }
    return fontSize * 0.55f;
}

inline float estimateTextWidth(const std::string& text, float fontSize) {
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        width += glyphAdvance(byte, fontSize);
        index += glyphLength(byte);
    }
    return width;
}

inline std::string elideToWidth(const std::string& text, float maxWidth, float fontSize) {
    if (maxWidth <= 0.0f) {
        return {};
    }
    if (estimateTextWidth(text, fontSize) <= maxWidth) return text;
    const float ellipsisWidth = fontSize;
    if (maxWidth < ellipsisWidth) return {};
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        const float advance = glyphAdvance(byte, fontSize);
        if (width + advance + ellipsisWidth > maxWidth) {
            return text.substr(0, index) + "\xE2\x80\xA6";
        }
        width += advance;
        index += glyphLength(byte);
    }
    return text;
}

// 工具栏按钮统一在这里收口；图标由应用内原生矢量路径绘制。
inline void toolButton(eui::Ui& ui,
                       const std::string& id,
                       const std::string& label,
                       float width,
                       bool highlighted,
                       bool enabled,
                       std::function<void()> onClick,
                       float fontSize = 14.0f,
                       UiIcon icon = UiIcon::None) {
    const EditorColors& colors = editorColors();
    eui::Color normal = transparentColor();
    eui::Color hover = colors.rowHover;
    eui::Color pressed = colors.pressed;
    eui::Color textColor = enabled ? colors.text : colors.textMuted;
    if (highlighted) {
        normal = rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.16f);
        textColor = colors.accent;
    }

    const float height = fontSize + 14.0f;
    const bool hasIcon = icon != UiIcon::None;
    const float glyph = std::min(fontSize, height - 8.0f);
    const float labelX = hasIcon ? 8.0f + glyph + 7.0f : 0.0f;
    ui.stack(id).size(width, height).content([&] {
        auto background = ui.rect(id + ".bg").fill().radius(6.0f)
            .states(normal, hover, pressed).disabled(!enabled).preserveFocusOnPress()
            .transition(quickTransition());
        if (enabled) background.cursor(eui::CursorShape::Hand);
        if (enabled && onClick) background.onClick(std::move(onClick));
        background.build();
        if (hasIcon) iconView(ui, id + ".icon", icon, 8.0f, (height-glyph)*.5f, glyph, textColor);
        ui.text(id + ".text").position(labelX, 0).size(std::max(0.0f, width-labelX), height)
            .text(label).fontSize(fontSize).color(textColor)
            .horizontalAlign(hasIcon ? eui::HorizontalAlign::Left : eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center).build();
    }).build();
}

} // namespace neo
