#pragma once

#include "components/theme.h"
#include "core/dsl.h"
#include "core/render/text.h"

#include <string>

namespace components {

using TextStyle = core::TextStyle;

inline TextStyle bodyTextStyle(const theme::ThemeColorTokens& tokens, const std::string& value = "") {
    TextStyle style;
    style.text = value;
    style.color = theme::pageVisuals(tokens).bodyColor;
    style.fontSize = theme::pageVisuals(tokens).labelSize;
    return style;
}

inline TextStyle titleTextStyle(const theme::ThemeColorTokens& tokens, const std::string& value = "") {
    TextStyle style;
    style.text = value;
    style.color = theme::pageVisuals(tokens).titleColor;
    style.fontSize = theme::pageVisuals(tokens).headerTitleSize;
    return style;
}

inline TextStyle subtitleTextStyle(const theme::ThemeColorTokens& tokens, const std::string& value = "") {
    TextStyle style;
    style.text = value;
    style.color = theme::pageVisuals(tokens).subtitleColor;
    style.fontSize = theme::pageVisuals(tokens).headerSubtitleSize;
    return style;
}

inline core::dsl::TextBuilder text(core::dsl::Ui& ui, const std::string& id) {
    auto builder = ui.text(id);
    builder.color(theme::dark().text);
    return builder;
}

inline core::dsl::TextBuilder text(core::dsl::Ui& ui, const std::string& id, const theme::ThemeColorTokens& tokens) {
    auto builder = ui.text(id);
    builder.color(tokens.text);
    return builder;
}

inline core::dsl::TextBuilder text(core::dsl::Ui& ui, const std::string& id, const TextStyle& style) {
    auto builder = ui.text(id);
    builder.text(style.text)
        .fontFamily(style.fontFamily)
        .fontSize(style.fontSize)
        .fontWeight(style.fontWeight)
        .color(style.color)
        .maxWidth(style.maxWidth)
        .wrap(style.wrap)
        .horizontalAlign(style.horizontalAlign)
        .verticalAlign(style.verticalAlign)
        .lineHeight(style.lineHeight);
    return builder;
}

} // namespace components
