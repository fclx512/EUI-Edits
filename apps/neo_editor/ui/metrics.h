#pragma once

#include "state/app_state.h"

#include <algorithm>
#include <cmath>

// 全应用的字号与尺寸只从这里取。
//
// 动手前这些数字是散在 6 个文件里的字面量（12/13/14/15/16），
// 结果是"设置面板里改字号"改不动任何东西。收口到这里之后，
// 字号才成为一个可变量，也只有这一个入口。
//
// 2026-09-22 补充：结构体里的默认值只是"派生失败时的兜底"，
// 实际值一律由 uiMetrics(state) 按 state.uiFontSize 算出来——
// 侧栏原先还在用 13/11/12 的字面量，改设置它一点不动，就是漏了这一步。
//
// 单位是逻辑像素：框架会再乘 effectiveScale（= 系统 DPI × uiScale）。
namespace neo {

struct UiMetrics {
    // 菜单栏
    float menuBarHeight = 34.0f;
    float menuFontSize = 14.0f;
    float menuItemHeight = 32.0f;
    float menuWidth = 212.0f;
    float menuTitlePadding = 10.0f;
    float menuTitleGap = 2.0f;

    // 状态栏
    float statusBarHeight = 26.0f;
    float statusFontSize = 13.0f;

    // 文档库侧栏。这里原先全是字面量（13/11/12/26/13），改设置里的字号它一点不动。
    float vaultTitleFontSize = 13.0f;
    float vaultPathFontSize = 11.0f;
    float vaultFilterFontSize = 12.0f;
    float vaultRowFontSize = 13.0f;
    // 行高必须固定：virtualList 的前提是等高行，改成自适应会毁掉虚拟滚动。
    float vaultRowHeight = 26.0f;
    float vaultIndentPerDepth = 14.0f;
    float vaultIconSize = 12.0f;

    // 应用原生矢量图标的通用尺寸。
    float iconSize = 14.0f;

    // 编辑区
    float editorFontSize = 16.0f;
    float editorInset = 18.0f;

    // 设置面板
    float panelFontSize = 14.0f;
    float panelHintFontSize = 12.0f;
    float panelLabelFontSize = 17.0f;
    float pageTitleFontSize = 28.0f;
    float sectionTitleFontSize = 22.0f;
    float panelControlHeight = 28.0f;
    float panelRowGap = 18.0f;
};

inline UiMetrics uiMetrics(const AppState& state) {
    UiMetrics metrics;
    // 编辑区字号与界面字号是两个独立旋钮（settings 里的 editor_font_size / ui_font_size）。
    // 界面的每一处字号都从界面字号派生，这样"设置里改字号界面对不上"不会再有第二次。
    metrics.editorFontSize = state.editorFontSize;

    const float ui = std::round(std::clamp(state.uiFontSize, kMinimumUiFontSize, kMaximumUiFontSize));
    metrics.menuFontSize = ui;
    metrics.menuBarHeight = std::max(30.0f, ui + 20.0f);
    metrics.menuItemHeight = std::max(28.0f, ui + 18.0f);

    metrics.statusFontSize = std::max(11.0f, ui - 1.0f);
    metrics.statusBarHeight = std::max(24.0f, ui + 12.0f);

    metrics.vaultTitleFontSize = std::max(11.0f, ui - 1.0f);
    metrics.vaultPathFontSize = std::max(10.0f, ui - 3.0f);
    metrics.vaultFilterFontSize = std::max(10.0f, ui - 2.0f);
    metrics.vaultRowFontSize = std::max(11.0f, ui - 1.0f);
    // 行高跟着字号走，但始终是固定值（virtualList 的前提）。
    metrics.vaultRowHeight = std::max(22.0f, std::round(ui * 1.85f));
    metrics.vaultIndentPerDepth = std::max(12.0f, std::round(ui * 1.0f));
    metrics.vaultIconSize = std::max(11.0f, ui - 1.0f);

    metrics.iconSize = std::max(12.0f, std::round(ui * 1.05f));

    metrics.panelFontSize = ui;
    // 设置卡片整体压矮后，标题与说明文字略微放大保住可读性。
    metrics.panelHintFontSize = std::max(11.0f, ui - 1.0f);
    metrics.panelLabelFontSize = ui + 4.0f;
    metrics.pageTitleFontSize = ui * 2.0f;
    metrics.sectionTitleFontSize = std::round(ui * 1.55f);

    return metrics;
}

} // namespace neo
