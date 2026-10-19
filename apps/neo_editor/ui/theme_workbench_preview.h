#pragma once

#include "components/markdown.h"
#include "components/scrollview.h"
#include "model/style_schema.h"
#include "ui/widgets.h"
#include "core/dsl.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

namespace neo::theme_workbench_preview {

// 样张使用生产 Markdown 组件和 style_schema.h 的排版换算；
// 场景命中、代码 token 与部分几何独立绘制，不代表完整编辑器的像素一致性。
inline components::MarkdownStyle draftMarkdownStyle(const EditorColors& colors, const ThemeTypography& t,
                                                    float bodyPx, const char* bodyFont, const char* codeFont) {
    components::MarkdownStyle style(colors.tokens);
    style.text = colors.text;
    style.heading = colors.heading;
    style.muted = colors.textMuted;
    style.accent = colors.markdownAccent;
    style.codeText = colors.codeText;
    style.codeBackground = colors.codeBackground;
    style.inlineCodeBackground = core::mixColor(colors.editor, colors.accent, .075f);
    style.inlineCodeBorder = core::mixColor(colors.editor, colors.accent, .28f);
    style.quoteBackground = colors.quoteBackground;
    style.divider = colors.divider;
    const char* body = bodyFont ? bodyFont : "";
    const char* code = codeFont ? codeFont : "monospace";
    if (t.bodyFontFamily && !t.bodyFontFamily->empty() &&
        (!*body || std::strcmp(body, kUiFontFamily) == 0)) body = t.bodyFontFamily->c_str();
    if (t.codeFontFamily && !t.codeFontFamily->empty() &&
        (!*code || std::strcmp(code, "monospace") == 0)) code = t.codeFontFamily->c_str();
    style.fontFamily = body;
    style.codeFontFamily = code;
    const float em = bodyPx;
    style.bodySize = em;
    style.bodyLineHeight = em * t.bodyLineHeight.value_or(kMarkdownBodyLineHeight);
    style.h1Size = em * t.h1SizeFactor.value_or(1.618f);
    style.h2Size = em * t.h2SizeFactor.value_or(1.462f);
    style.h3Size = em * t.h3SizeFactor.value_or(1.318f);
    style.h1LineHeight = style.h1Size * t.h1LineHeightFactor.value_or(1.2f) + em;
    style.h2LineHeight = style.h2Size * t.h2LineHeightFactor.value_or(1.2f) + em;
    style.h3LineHeight = style.h3Size * t.h3LineHeightFactor.value_or(1.2f) + em;
    style.h1TextLineHeight = style.h1Size * t.h1LineHeightFactor.value_or(1.2f);
    style.h2TextLineHeight = style.h2Size * t.h2LineHeightFactor.value_or(1.2f);
    style.h3TextLineHeight = style.h3Size * t.h3LineHeightFactor.value_or(1.3f);
    style.headingSpaceBefore = em * t.headingSpaceBefore.value_or(1.0f);
    style.tableSpaceBefore = em * t.tableSpaceBefore.value_or(1.0f);
    style.codeSize = em * t.codeSizeFactor.value_or(kMarkdownCodeSizeFactor);
    style.codeLineHeight = style.codeSize * kMarkdownCodeLineHeight;
    style.blockGap = em * t.blockGap.value_or(kMarkdownBlockGap);
    style.listIndent = em * t.listIndent.value_or(kMarkdownListIndent);
    style.radius = em * t.radius.value_or(kMarkdownRadius);
    return style;
}

struct Args {
    const EditorColors* colors = nullptr;
    const ThemeTypography* typography = nullptr;
    float bodyPx = 16.0f;
    const char* bodyFont = "";
    const char* codeFont = "";
    bool codeView = false;
    bool interactive = true;
    std::string selected;
    std::string hovered;
    eui::Color marker;   // 标记色取应用当前强调色，不随草稿变化，任何配色下都看得见。
    float* scroll = nullptr;
    float* sidebarScroll = nullptr;
    std::function<void(const std::string&)> onSelect;
    std::function<void(const std::string&, bool)> onHover;
};

// 本地只读文稿场景：不创建编辑器输入状态、不打开用户文档、不激活主题。
// 悬停显示细描边，选中显示粗描边；描边只落在被点中的那一项上。
inline void drawContent(eui::Ui& ui, const std::string& id, float x, float y, float width, float height, const Args& a) {
    const EditorColors& colors = *a.colors;
    const float sidebar = std::clamp(width * .24f, 120.0f, 168.0f);
    const float top = 30.0f, tools = 38.0f, status = 26.0f;
    const float contentY = y + top + tools;
    const float contentH = std::max(1.0f, height - top - tools - status);
    const float paperX = x + sidebar + 1.0f;
    const float paperW = std::max(80.0f, width - sidebar - 2.0f);
    const auto mark = [&](const std::string& key) -> std::pair<float, eui::Color> {
        if (!a.interactive || key.empty()) return {0.0f, transparentColor()};
        if (a.selected == key) return {2.0f, a.marker};
        if (a.hovered == key) return {1.0f, rgba(a.marker.r, a.marker.g, a.marker.b, .6f)};
        return {0.0f, transparentColor()};
    };
    const auto surface = [&](eui::Ui& target, const std::string& suffix, const std::string& key,
                             float px, float py, float pw, float ph, eui::Color fill, float radius = 0.0f) {
        const auto [bw, bc] = mark(key);
        auto area = target.rect(id + suffix);
        area.position(px, py).size(std::max(1.0f, pw), std::max(1.0f, ph)).radius(radius)
            .states(fill, fill, fill).border(bw, bc);
        if (a.interactive) {
            const auto onSelect = a.onSelect;
            const auto onHover = a.onHover;
            area.cursor(eui::CursorShape::Hand).preserveFocusOnPress()
                .onClick([onSelect, key] { if (onSelect) onSelect(key); })
                .onHover([onHover, key](bool inside) { if (onHover) onHover(key, inside); });
        }
        area.build();
    };
    const auto text = [&](eui::Ui& target, const std::string& suffix, const std::string& value,
                          float px, float py, float pw, float ph, eui::Color fg, float size = 12.0f) {
        target.text(id + suffix).position(px, py).size(std::max(1.0f, pw), std::max(1.0f, ph))
            .text(value).fontSize(size).color(fg).verticalAlign(eui::VerticalAlign::Center).build();
    };

    surface(ui, ".window", "window", x, y, width, height, colors.window, 7.0f);
    text(ui, ".title", "EUI-Edits  ·  雨后笔记", x + 12, y + 2, width - 24, top - 4, colors.textMuted, 12);
    surface(ui, ".toolbar", "toolbar", x + 1, y + top, width - 2, tools, colors.toolbar);
    surface(ui, ".toolbar.border", "border", x + 1, y + top + tools - 1, width - 2, 1, colors.border);
    surface(ui, ".tab", "editor", x + 10, y + top + 6, 108, tools - 12, colors.editor, 5.0f);
    text(ui, ".tab.text", "雨后笔记.md", x + 19, y + top + 6, 96, tools - 12, colors.text, 12);
    const float newW = std::min(86.0f, width * .17f);
    const float newX = width - newW - 10;
    if (newX >= 182) {
        surface(ui, ".tool", "text", x + 128, y + top + 7, 46, tools - 14, colors.toolbar, 5.0f);
        text(ui, ".tool.text", "编辑", x + 139, y + top + 7, 35, tools - 14, colors.text, 12);
    }
    if (newX >= 244) {
        surface(ui, ".pressed", "pressed", x + 178, y + top + 7, 58, tools - 14, colors.pressed, 5.0f);
        text(ui, ".pressed.text", "已按下", x + 187, y + top + 7, 45, tools - 14, colors.text, 12);
    }
    surface(ui, ".new", "accent", x + width - newW - 10, y + top + 6, newW, tools - 12, colors.accent, 5.0f);
    ui.text(id + ".new.text").position(x + width - newW - 10, y + top + 6).size(newW, tools - 12)
        .text("新建笔记").fontSize(12).color(colors.editor)
        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();

    surface(ui, ".sidebar", "panel", x + 1, contentY, sidebar, contentH, colors.panel);
    surface(ui, ".sidebar.border", "border", x + sidebar, contentY, 1, contentH, colors.border);
    struct Row { UiIcon icon; const char* name; const char* key; };
    const Row rows[] = {
        {UiIcon::Folder, "写作项目", "text"}, {UiIcon::MarkdownFile, "雨后笔记.md", "rowActive"},
        {UiIcon::TextFile, "提纲.txt", "rowHover"}, {UiIcon::CodeFile, "片段.cpp", "text"},
        {UiIcon::DataFile, "资料.json", "text"}
    };
    const eui::Color iconColors[] = {colors.iconFolder, colors.iconMd, colors.iconTxt, colors.iconCode, colors.iconData};
    float* sidebarScroll = a.sidebarScroll;
    components::scrollView(ui, id + ".sidebar.viewport").theme(colors.tokens)
        .position(x + 1, contentY).size(sidebar - 1, contentH).scrollbarWidth(4).scrollbarGap(0)
        .offset(sidebarScroll ? *sidebarScroll : 0).step(30)
        .onChange([sidebarScroll](float value) { if (sidebarScroll) *sidebarScroll = value; })
        .content([&](eui::Ui& side, float availableWidth, float) {
            side.stack(id + ".sidebar.rows").size(availableWidth, 190).content([&] {
                surface(side, ".sidebar.title", "textMuted", 7, 8, availableWidth - 14, 24, colors.panel, 4);
                text(side, ".sidebar.title.text", "文档库", 13, 8, availableWidth - 22, 24, colors.textMuted, 11);
                for (int i = 0; i < 5; ++i) {
                    const eui::Color bg = i == 1 ? colors.rowActive : i == 2 ? colors.rowHover : colors.panel;
                    const float rowY = 38 + i * 30.0f;
                    surface(side, ".file." + std::to_string(i), rows[i].key, 7, rowY, availableWidth - 14, 27, bg, 4);
                    if (rows[i].icon == UiIcon::Folder)
                        iconView(side, id + ".file.icon." + std::to_string(i), UiIcon::Folder, 13, rowY + 6.5f, 14, iconColors[i]);
                    else
                        side.rect(id + ".file.icon." + std::to_string(i)).position(15, rowY + 7).size(10, 13)
                            .radius(2).color(iconColors[i]).build();
                    text(side, ".file.name." + std::to_string(i), rows[i].name, 33, rowY, availableWidth - 43, 27, colors.text, 12);
                }
            }).build();
        }).build();

    surface(ui, ".editor", "editor", paperX, contentY, paperW, contentH, colors.editor);
    const float pad = std::clamp(paperW * .06f, 14.0f, 36.0f);
    const auto style = draftMarkdownStyle(colors, *a.typography, std::max(10.0f, a.bodyPx), a.bodyFont, a.codeFont);
    const std::string articleParts[][2] = {
        {"heading", "# 雨后，留给写作的光"},
        {"text", "清晨的窗边有一小段安静。把它记下来，就像给匆忙的日子留一处停顿，随后继续写。文字顺着想法铺开，不需要先决定它最终会去往哪里。"},
        {"heading", "## 把想法放在纸上"},
        {"text", "先从一行文字开始，再慢慢铺开。一个合适的主题应该让内容清晰，也让界面保持安静。"},
        {"markdownAccent", "[阅读笔记](#)"},
        {"quoteBackground", "> 让颜色服务于阅读，而不是遮住内容。"},
        {"heading", "### 下一段"},
        {"text", "看见重点，也看见层次。主题不是一张孤立的色卡，文件库、工具栏和长段落共同决定阅读的感受。"},
    };
    const std::string codeParts[][2] = {
        {"heading", "# 一份可继续修改的笔记"},
        {"text", "引用、代码与普通正文使用各自的语义颜色。这一页把想法写成一段小小的程序。"},
        {"quoteBackground", "> 让颜色服务于阅读，而不是遮住内容。"},
        {"divider", "---"},
        {"syntax", ""},
        {"text", "颜色落在每一个词上，也落在代码块与文稿的留白里。"}
    };
    const std::string (*parts)[2] = a.codeView ? codeParts : articleParts;
    const int partCount = a.codeView ? 6 : 8;
    float* scroll = a.scroll;
    components::scrollView(ui, id + ".document")
        .theme(colors.tokens).position(paperX + 1, contentY + 1)
        .size(std::max(1.0f, paperW - 2), std::max(1.0f, contentH - 2))
        .offset(scroll ? *scroll : 0.0f).step(std::max(20.0f, style.bodyLineHeight * 2))
        .onChange([scroll](float value) { if (scroll) *scroll = value; })
        .content([&](eui::Ui& doc, float availableWidth, float) {
            doc.rect(id + ".document.topgap").size(availableWidth, 14).color(colors.editor).build();
            const float wrapWidth = std::max(60.0f, availableWidth - pad * 2);
            for (int i = 0; i < partCount; ++i) {
                const std::string source = parts[i][1], key = parts[i][0];
                if (key == "syntax") {
                    const float lh = std::max(20.0f, style.codeLineHeight);
                    const float cs = std::max(10.0f, style.codeSize);
                    const float blockH = lh * 3 + 16;
                    doc.stack(id + ".syntax.block").size(availableWidth, blockH + 4).content([&] {
                        surface(doc, ".syntax.surface", "codeBackground", pad, 0, wrapWidth, blockH,
                                colors.codeBackground, std::max(0.0f, style.radius));
                        struct Token { const char* key; const char* value; float col; int line; };
                        // col 以字符宽计；等宽字体下约 0.6em。
                        const Token tokens[] = {
                            {"tokenComment", "// theme.cpp", 0, 0},
                            {"tokenKeyword", "const", 0, 1}, {"tokenProperty", "theme", 6, 1},
                            {"tokenOperator", "=", 12, 1}, {"tokenFunction", "makeTheme", 14, 1}, {"codeText", "(", 23, 1},
                            {"tokenString", "\"forest\"", 2, 2}, {"codeText", ",", 10, 2},
                            {"tokenNumber", "3", 12, 2}, {"codeText", ");", 13, 2},
                        };
                        const float cw = cs * .6f;
                        for (int ti = 0; ti < 10; ++ti) {
                            const auto* f = findThemeColorField(tokens[ti].key);
                            const eui::Color fg = f ? colors.*(f->member) : colors.codeText;
                            const float tx = pad + 12 + tokens[ti].col * cw;
                            const float tw = std::max(cw, static_cast<float>(std::string(tokens[ti].value).size()) * cw + 2);
                            if (tx + tw > pad + wrapWidth - 4) continue;
                            surface(doc, ".syntax.token." + std::to_string(ti), tokens[ti].key,
                                    tx - 2, 8 + tokens[ti].line * lh, tw + 4, lh, colors.codeBackground, 3.0f);
                            doc.text(id + ".syntax.text." + std::to_string(ti)).position(tx, 8 + tokens[ti].line * lh)
                                .size(tw, lh).text(tokens[ti].value).fontSize(cs).fontFamily(style.codeFontFamily)
                                .color(fg).verticalAlign(eui::VerticalAlign::Center).build();
                        }
                    }).build();
                    doc.rect(id + ".syntax.gap").size(availableWidth, std::max(4.0f, style.blockGap * .6f))
                        .color(colors.editor).build();
                    continue;
                }
                const float blockHeight = std::max(22.0f, components::MarkdownBuilder::estimateHeight(source, wrapWidth, style) + 2.0f);
                doc.stack(id + ".block." + std::to_string(i)).size(availableWidth, blockHeight)
                    .content([&, source, key, i, blockHeight] {
                        components::markdown(doc, id + ".md." + std::to_string(i))
                            .position(pad, 0).width(wrapWidth).wrapContentHeight()
                            .markdown(source).style(style).build();
                        surface(doc, ".hit." + std::to_string(i), key, pad - 4, 0, wrapWidth + 8, blockHeight,
                                transparentColor(), 4.0f);
                    }).build();
                doc.rect(id + ".block.gap." + std::to_string(i)).size(availableWidth, std::max(4.0f, style.blockGap * .6f))
                    .color(colors.editor).build();
                if (i == 1 && !a.codeView) {
                    const float lh = std::max(22.0f, style.bodyLineHeight);
                    doc.stack(id + ".selection.row").size(availableWidth, lh + 6).content([&] {
                        const float sw = std::min(style.bodySize * 7.6f, wrapWidth - 10);
                        surface(doc, ".selection.hit", "selectionBackground", pad, 0, sw, lh, colors.selectionBackground, 2.0f);
                        doc.text(id + ".selection.text").position(pad + 2, 0).size(sw, lh).text("选中的一段正文")
                            .fontSize(style.bodySize).fontFamily(style.fontFamily).color(colors.text)
                            .verticalAlign(eui::VerticalAlign::Center).build();
                        surface(doc, ".caret.hit", "accent", pad + sw + 3, 3, 2, lh - 6, colors.accent);
                    }).build();
                }
            }
            doc.rect(id + ".document.bottomgap").size(availableWidth, 18).color(colors.editor).build();
        }).build();
    surface(ui, ".status", "statusBar", x + 1, y + height - status, width - 2, status - 1, colors.statusBar, 6.0f);
    surface(ui, ".status.border", "border", x + 1, y + height - status, width - 2, 1, colors.border);
    text(ui, ".status.text", "第 12 行  ·  UTF-8  ·  Markdown", x + 12, y + height - status,
         width - 24, status - 1, colors.textMuted, 11);
    // 外框最后画，压在内部色块上；选中"边框"时也走这里的粗描边。
    const auto [bw, bc] = mark("border");
    ui.rect(id + ".outer.border").position(x, y).size(width, height).radius(7.0f)
        .color(transparentColor()).border(bw > 0 ? bw : 1.0f, bw > 0 ? bc : colors.border).build();
}

inline void draw(eui::Ui& ui, const std::string& id, float x, float y, float width, float height, const Args& a) {
    ui.stack(id + ".viewport").position(x, y).size(width, height).clip().content([&] {
        drawContent(ui, id, 0, 0, width, height, a);
    }).build();
}

} // namespace neo::theme_workbench_preview
