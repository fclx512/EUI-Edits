#pragma once

#include "model/i18n.h"

#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "ui/icons.h"

#include <algorithm>
#include <string>

namespace neo {
namespace status_detail {

// 一个图标位：和它后面的文字用同一个 ui.row 的 gap 隔开，
// 所以宽度只留字形本身，间距交给 row。
inline void statusIcon(eui::Ui& ui,
                       const std::string& id,
                       UiIcon icon,
                       float fontSize,
                       float height,
                       eui::Color color) {
    const float size = std::min(fontSize + 1.0f, height - 2.0f);
    iconView(ui, id, icon, (fontSize + 2.0f - size)*.5f, (height-size)*.5f, size, color);
}

inline void statusField(eui::Ui& ui,
                        const std::string& id,
                        const char* fontFamily,
                        const std::string& value,
                        float fontSize,
                        float height,
                        eui::Color color) {
    ui.text(id)
        .size(core::TextPrimitive::measureTextWidth(value, fontFamily, fontSize) + 4.0f, height)
        .text(value)
        .fontFamily(fontFamily)
        .fontSize(fontSize)
        .color(color)
        .verticalAlign(eui::VerticalAlign::Center)
        .clip()
        .build();
}

inline std::string statusTextForWidth(const std::string& text,
                                      float width,
                                      float fontSize,
                                      const char* fontFamily) {
    if (core::TextPrimitive::measureTextWidth(text, fontFamily, fontSize) <= width) {
        return text;
    }
    return elideToWidth(text, width, fontSize);
}

inline std::string statusNameForWidth(const std::string& name,
                                      bool dirty,
                                      float width,
                                      float fontSize,
                                      const char* fontFamily) {
    const float markerWidth = dirty ? fontSize * .55f + 6.0f : 0.0f;
    if (dirty && width < markerWidth) {
        return {};
    }
    return statusTextForWidth(name, std::max(0.0f, width - markerWidth), fontSize, fontFamily);
}

} // namespace status_detail

inline void statusBarView(eui::Ui& ui, AppState& state, float width) {
    using namespace status_detail;
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    const float height = metrics.statusBarHeight;
    const float fontSize = metrics.statusFontSize;
    const char* uiFont = uiFontFamily(state);

    // 行数/字符数按 revision 缓存，避免每次 compose 都扫一遍全文。
    if (state.statsRevision != state.revision) {
        state.stats = textfile::measure(state.doc.text);
        state.statsRevision = state.revision;
    }

    const std::string nameText = state.displayName();
    std::string locationText;
    if (state.recovered) {
        locationText = state.recoveredFrom.empty() ? std::string(i18n::tr("status.recovered"))
                                                  : i18n::format("status.recovered_from", {{"path", state.recoveredFrom}});
    } else if (state.path.empty()) {
        locationText = i18n::tr("status.unsaved");
    } else {
        locationText = state.path;
    }

    if (state.recoveryWriteWarning) locationText = i18n::tr("tabs.recovery_unavailable");

    // 编码按文档记录的实际编码显示（UTF-8/UTF-16 LE/GBK/…），BOM 与换行风格随后。
    const std::string metaText = textfile::encodingLabel(state.doc) +
                                 (state.doc.hadBom ? std::string(" BOM") : std::string()) + " · " +
                                 (state.doc.lineEnding == textfile::LineEnding::CrLf ? "CRLF" : "LF");
    const std::string linesText = i18n::format("status.lines", {{"count", std::to_string(state.stats.lines)}});
    const std::string charsText = i18n::format("status.characters", {{"count", std::to_string(state.stats.characters)}});

    constexpr float kGap = 6.0f;
    const float iconSlot = fontSize + 2.0f;
    const float horizontalPad = std::min(12.0f, std::max(1.0f, width * 0.04f));
    // 宽度一律用真实字体测量（estimateTextWidth 会低估大写字母，文档名溢出
    // 压过段间距是状态栏重叠的根因）；右段各字段先量一次缓存，避免级联
    // 收缩判断里反复 shaping。
    const auto measure = [&](const std::string& value) {
        return core::TextPrimitive::measureTextWidth(value, uiFont, fontSize);
    };
    const auto fieldWidth = [&](const std::string& value) {
        return measure(value) + 4.0f;
    };
    const float metaWidth = fieldWidth(metaText);
    const float linesWidth = fieldWidth(linesText);
    const float charsWidth = fieldWidth(charsText);
    const float nameTextWidth = measure(nameText);
    const float minimumNameWidth = std::min(
        nameTextWidth + (state.dirty() ? (fontSize*.55f+6) : 0.0f),
        std::max(40.0f, fontSize * 3.0f));
    const float leftMinimum = horizontalPad * 2.0f + minimumNameWidth;
    const float languageWidth=std::min(120.0f,measure(filetypes::label(state.language())+" ▾")+14.0f);
    const bool wrapControl=width>=480;
    const float viewControls=languageWidth+(wrapControl?64.0f:0.0f);
    const auto rightWidthFor = [&](bool meta, bool lines, bool chars, bool icons) {
        const int count = static_cast<int>(meta) + static_cast<int>(lines) + static_cast<int>(chars);
        if (count == 0) {
            return viewControls+horizontalPad*2;
        }
        float result = horizontalPad * 2.0f + viewControls + kGap;
        if (meta) result += metaWidth;
        if (lines) result += linesWidth;
        if (chars) result += charsWidth;
        if (icons) result += count * iconSlot;
        result += kGap * (icons ? count * 2 - 1 : count - 1);
        return result;
    };

    // Keep encoding and character count first. Drop line count and icons as space tightens;
    // hide a whole value rather than letting neighboring fields overlap or truncate numbers.
    bool showMeta = true;
    bool showLines = true;
    bool showChars = true;
    bool showRightIcons = true;
    const float availableRight = std::max(0.0f, width - leftMinimum);
    if (rightWidthFor(showMeta, showLines, showChars, showRightIcons) > availableRight) {
        showLines = false;
    }
    if (rightWidthFor(showMeta, showLines, showChars, showRightIcons) > availableRight) {
        showRightIcons = false;
    }
    if (rightWidthFor(showMeta, showLines, showChars, showRightIcons) > availableRight) {
        showMeta = false;
    }
    if (!showLines &&
        rightWidthFor(showMeta, true, showChars, showRightIcons) > availableRight &&
        showRightIcons &&
        rightWidthFor(showMeta, true, showChars, false) <= availableRight) {
        showRightIcons = false;
    }
    if (!showLines && rightWidthFor(showMeta, true, showChars, showRightIcons) <= availableRight) {
        showLines = true;
    }
    if (rightWidthFor(showMeta, showLines, showChars, showRightIcons) > availableRight) {
        showChars = false;
        showMeta = true;
    }
    if (rightWidthFor(showMeta, showLines, showChars, showRightIcons) > availableRight) {
        showMeta = false;
    }
    const float rightWidth = rightWidthFor(showMeta, showLines, showChars, showRightIcons);
    const float leftWidth = std::max(0.0f, width - rightWidth);
    const float leftInner = std::max(0.0f, leftWidth - horizontalPad * 2.0f);
    const float nameMinimum = std::min(minimumNameWidth, leftInner);
    const bool showFileIcon = leftInner >= iconSlot + kGap + nameMinimum && nameMinimum > 0.0f;
    const float afterFileIcon = std::max(0.0f, leftInner - (showFileIcon ? iconSlot + kGap : 0.0f));
    const float locationMinimum = std::min(48.0f, measure(locationText));
    const float pathFixed = kGap + iconSlot + kGap;
    const bool showLocation = afterFileIcon >= nameMinimum + pathFixed + locationMinimum &&
                              locationMinimum > 0.0f;
    float nameWidth = afterFileIcon;
    float locationWidth = 0.0f;
    if (showLocation) {
        const float flexible = afterFileIcon - pathFixed;
        nameWidth = std::min(nameTextWidth + (state.dirty() ? (fontSize*.55f+6) : 0.0f),
                             std::max(nameMinimum, flexible * 0.55f));
        locationWidth = std::max(0.0f, flexible - nameWidth);
    }
    const float renderedRightLeft = leftWidth;

    ui.stack("status")
        .size(width, height)
        .content([&] {
            ui.rect("status.bg")
                .fill()
                .color(colors.statusBar)
                .build();
            ui.rect("status.separator")
                .position(0.0f, 0.0f)
                .size(width, 1.0f)
                .color(colors.border)
                .build();

            ui.row("status.left")
                .position(0.0f, 0.0f)
                .size(leftWidth, height)
                .padding(horizontalPad, 0.0f)
                .gap(kGap)
                .alignItems(eui::Align::CENTER)
                .justifyContent(eui::Align::START)
                .zIndex(1)
                .content([&] {
                    if (showFileIcon) {
                        ui.stack("status.name.icon").size(fontSize + 2.0f, height).content([&] {
                            const auto kind=state.path.empty()?filetypes::detect(state.markdownCapable()?"new.md":"new.txt").category:filetypes::detect(state.path).category;
                            const auto icon=kind==filetypes::Category::Markdown?UiIcon::MarkdownFile:kind==filetypes::Category::Text?UiIcon::TextFile:kind==filetypes::Category::Code?UiIcon::CodeFile:kind==filetypes::Category::Data?UiIcon::DataFile:UiIcon::File;
                            const auto color=kind==filetypes::Category::Markdown?colors.iconMd:kind==filetypes::Category::Text?colors.iconTxt:kind==filetypes::Category::Code?colors.iconCode:kind==filetypes::Category::Data?colors.iconData:colors.iconFile;
                            iconView(ui,"status.name.glyph",icon,0,(height-fontSize-2)*.5f,fontSize+2,color);
                        }).build();
                    }
                    const std::string renderedName = statusNameForWidth(nameText, state.dirty(), nameWidth, fontSize, uiFont);
                    ui.stack("status.name").size(nameWidth, height).content([&] {
                        ui.text("status.name.text").fill().text(renderedName)
                            .fontFamily(uiFont).fontSize(fontSize)
                            .color(state.dirty() ? colors.accent : colors.text)
                            .verticalAlign(eui::VerticalAlign::Center).clip().build();
                        if (state.dirty() && nameWidth >= 6) {
                            const float dotX = std::min(nameWidth-6,
                                core::TextPrimitive::measureTextWidth(renderedName, uiFont, fontSize)+5);
                            ui.rect("status.name.dirty").position(dotX, (height-5)*.5f)
                                .size(5,5).radius(2.5f).color(colors.accent).build();
                        }
                    }).build();
                    if (showLocation) {
                        statusIcon(ui, "status.location.icon", UiIcon::FolderOpen, fontSize, height, colors.textMuted);
                        ui.text("status.location")
                            .size(locationWidth, height)
                            .text(statusTextForWidth(locationText, locationWidth, fontSize, uiFont))
                            .fontFamily(uiFont)
                            .fontSize(fontSize)
                            .color(colors.textMuted)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .clip()
                            .build();
                    }
                })
                .build();

            if (rightWidth > 0.0f) {
                ui.row("status.right")
                    .position(renderedRightLeft, 0.0f)
                    .size(rightWidth, height)
                    .padding(horizontalPad, 0.0f)
                    .gap(kGap)
                    .alignItems(eui::Align::CENTER)
                    .justifyContent(eui::Align::END)
                    .zIndex(1)
                    .content([&] {
                        ui.stack("status.language").size(languageWidth,height).content([&] {
                            ui.rect("status.language.hit").fill().states(transparentColor(),colors.rowHover,colors.pressed).cursor(eui::CursorShape::Hand).preserveFocusOnPress().onClick([&state]{state.languageMenuOpen=!state.languageMenuOpen;app::requestUpdate();}).build();
                            statusField(ui,"status.language.text",uiFont,filetypes::label(state.language())+" ▾",fontSize,height,colors.text);
                        }).build();
                        if(wrapControl) ui.stack("status.wrap").size(58,height).content([&] {
                            ui.rect("status.wrap.hit").fill().states(transparentColor(),colors.rowHover,colors.pressed).cursor(eui::CursorShape::Hand).preserveFocusOnPress().onClick([&state]{state.wrapOverride=state.wordWrap()?0:1;app::requestUpdate();}).build();
                            statusField(ui,"status.wrap.text",uiFont,state.wordWrap()?i18n::tr("status.wrap_on"):i18n::tr("status.wrap_off"),fontSize,height,colors.textMuted);
                        }).build();
                        if (showMeta) {
                            if (showRightIcons) statusIcon(ui, "status.meta.icon", UiIcon::Globe, fontSize, height, colors.textMuted);
                            statusField(ui, "status.meta", uiFont, metaText, fontSize, height, colors.textMuted);
                        }
                        if (showLines) {
                            if (showRightIcons) statusIcon(ui, "status.lines.icon", UiIcon::List, fontSize, height, colors.textMuted);
                            statusField(ui, "status.lines", uiFont, linesText, fontSize, height, colors.textMuted);
                        }
                        if (showChars) {
                            if (showRightIcons) statusIcon(ui, "status.chars.icon", UiIcon::Font, fontSize, height, colors.textMuted);
                            statusField(ui, "status.chars", uiFont, charsText, fontSize, height, colors.textMuted);
                        }
                    })
                    .build();
            }
        })
        .build();
}

} // namespace neo
