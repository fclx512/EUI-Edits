#pragma once

#include "eui_neo.h"
#include "state/app_state.h"
#include "ui/document_icon_geometry.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace neo {

// Application-owned vector icons, drawn by the native renderer at the current DPI.
// No bitmap cache or dependency on the UI library's icon font.
enum class UiIcon {
    None, Search, Outline, File, TextFile, MarkdownFile, CodeFile, DataFile, ImageFile,
    ChevronRight, ChevronDown, Previous, Next, Close, Replace,
    Folder, FolderOpen, ArrowLeft, ArrowRight,
    Globe, List, Font, Settings, Plus, Minus, Check, Info, Warning, Sliders,
    Heading, Bold, Italic, Link, Image, Task, Table, Code, Typewriter, Focus, Maximize, Restore, Pin
};



inline void iconView(eui::Ui& ui, const std::string& id, UiIcon icon,
                     float x, float y, float size, eui::Color color) {
    ui.stack(id).position(x, y).size(size, size).content([&] {
        const float scale = size / 24.0f;
        int segment = 0;
        const auto stroke = [&](float x1, float y1, float x2, float y2, float thickness) {
            const float length = std::hypot(x2 - x1, y2 - y1);
            if (length < 1e-4f) return;
            const float half = thickness * 0.5f;
            const float dx = (y2 - y1) / length * half;
            const float dy = (x1 - x2) / length * half;
            ui.polygon(id + ".line." + std::to_string(segment++))
                .size(size, size)
                .points({{(x1 + dx)*scale, (y1 + dy)*scale},
                         {(x2 + dx)*scale, (y2 + dy)*scale},
                         {(x2 - dx)*scale, (y2 - dy)*scale},
                         {(x1 - dx)*scale, (y1 - dy)*scale}})
                .color(color).build();
        };
        const auto line = [&](float x1, float y1, float x2, float y2) {
            stroke(x1, y1, x2, y2, 1.7f);
        };
        const auto dot = [&](const std::string& part, float cx, float cy, float diameter) {
            ui.rect(id + "." + part).position((cx-diameter*.5f)*scale, (cy-diameter*.5f)*scale)
                .size(diameter*scale, diameter*scale).radius(diameter*.5f*scale)
                .color(color).build();
        };
        const auto polygon = [&](const std::string& part, const auto& points,
                                 eui::Color fill) {
            std::vector<core::Vec2> scaled;
            scaled.reserve(points.size());
            for (const core::Vec2& point : points) {
                scaled.push_back({point.x * scale, point.y * scale});
            }
            ui.polygon(id + "." + part).size(size, size).points(std::move(scaled))
                .color(fill).build();
        };
        // Geometry is generated from the same master as the Windows ICO/SVG.
        // Theme colors remain live; only the Windows shell uses a fixed palette.
        const auto document = [&](UiIcon kind) {
            const EditorColors& colors = editorColors();
            const eui::Color edge = colors.iconFile;
            const eui::Color markColor = kind == UiIcon::CodeFile ? colors.iconCode
                : kind == UiIcon::DataFile ? colors.iconData
                : kind == UiIcon::File ? colors.text : color;
            polygon("page", document_icon::pageOuter, edge);
            polygon("paper", document_icon::pageInner, colors.editor);
            polygon("fold.edge", document_icon::foldEdge, edge);
            polygon("fold.paper", document_icon::foldPaper, colors.editor);
            polygon("fold.tint", document_icon::foldPaper, rgba(edge.r, edge.g, edge.b, .16f));
            const auto rules = [&](const auto& positions, float thickness) {
                for (float y : positions) {
                    ui.rect(id + ".rule." + std::to_string(segment++))
                        .position(document_icon::ruleLeft*scale, (y-thickness*.5f)*scale)
                        .size((document_icon::ruleRight-document_icon::ruleLeft)*scale, thickness*scale)
                        .radius(.2f*scale).color(markColor).build();
                }
            };
            if (kind == UiIcon::TextFile) polygon("mark", document_icon::txtMark, markColor);
            else if (kind == UiIcon::MarkdownFile) polygon("mark", document_icon::mdMark, markColor);
            else if (kind == UiIcon::CodeFile) {
                polygon("mark.left", document_icon::codeLeft, markColor);
                polygon("mark.right", document_icon::codeRight, markColor);
            } else if (kind == UiIcon::DataFile) {
                polygon("mark.left", document_icon::dataLeft, markColor);
                polygon("mark.right", document_icon::dataRight, markColor);
            } else if (kind == UiIcon::ImageFile) {
                // Application-only photograph mark on the existing folded page.
                // It does not add a Windows ICO/resource or association family.
                ui.rect(id + ".photo.frame").position(5.7f*scale, 9.8f*scale)
                    .size(11.9f*scale, 9.1f*scale).radius(.8f*scale)
                    .color(rgba(markColor.r,markColor.g,markColor.b,.12f))
                    .border(.9f*scale,markColor).build();
                const std::array<core::Vec2,5> mountains{{{6.8f,17.7f},{10.0f,13.4f},
                    {12.0f,15.5f},{14.2f,12.8f},{16.6f,17.7f}}};
                polygon("photo.mountains",mountains,markColor);
                ui.rect(id + ".photo.sun").position(7.3f*scale, 11.0f*scale)
                    .size(1.8f*scale,1.8f*scale).radius(.9f*scale).color(markColor).build();
            }
            if (kind == UiIcon::TextFile) rules(document_icon::txtRules, document_icon::ruleThickness);
            else if (kind == UiIcon::MarkdownFile) rules(document_icon::mdRules, document_icon::ruleThickness);
            else if (kind == UiIcon::CodeFile) rules(document_icon::codeRules, document_icon::ruleThickness);
            else if (kind == UiIcon::DataFile) rules(document_icon::dataRules, document_icon::ruleThickness);
            else if (kind == UiIcon::File) rules(document_icon::fileRules, document_icon::ruleThickness);
        };
        switch (icon) {
            case UiIcon::Pin:
                line(8, 4, 16, 4); line(9, 4, 9, 11); line(15, 4, 15, 11);
                line(9, 11, 6, 15); line(15, 11, 18, 15); line(6, 15, 18, 15);
                line(12, 15, 12, 21); break;
            case UiIcon::Heading:
                line(4, 5, 4, 19); line(14, 5, 14, 19); line(4, 12, 14, 12);
                line(18, 14, 20, 12); line(20, 12, 20, 20); break;
            case UiIcon::Bold:
                // One consistent weight, rounded bowls and joined stroke ends.
                stroke(6, 4, 6, 20, 2.2f);
                for (int bowl = 0; bowl < 2; ++bowl) {
                    const float cy = bowl == 0 ? 8.0f : 16.0f;
                    const float rx = bowl == 0 ? 4.5f : 5.5f;
                    stroke(6, cy-4, 12, cy-4, 2.2f);
                    stroke(6, cy+4, 12, cy+4, 2.2f);
                    float px=12, py=cy-4;
                    for (int step=1; step<=12; ++step) {
                        const float a=-1.5707963f + step*3.1415926f/12;
                        const float nx=12+rx*std::cos(a), ny=cy+4*std::sin(a);
                        stroke(px,py,nx,ny,2.2f);
                        dot("bold.join."+std::to_string(bowl)+"."+std::to_string(step),nx,ny,2.2f);
                        px=nx; py=ny;
                    }
                }
                break;
            case UiIcon::Italic:
                line(10, 4, 20, 4); line(4, 20, 14, 20); line(15, 4, 9, 20); break;
            case UiIcon::Link:
                line(9, 15, 15, 9); line(6, 10, 3, 13); line(3, 13, 3, 17);
                line(3, 17, 7, 21); line(7, 21, 11, 21); line(11, 21, 14, 18);
                line(10, 6, 13, 3); line(13, 3, 17, 3); line(17, 3, 21, 7);
                line(21, 7, 21, 11); line(21, 11, 18, 14); break;
            case UiIcon::Task:
                line(19, 12, 19, 20); line(19, 20, 4, 20); line(4, 20, 4, 5);
                line(4, 5, 13, 5); line(9, 10, 13, 14); line(13, 14, 21, 4); break;
            case UiIcon::Table:
                line(3, 4, 21, 4); line(21, 4, 21, 20); line(21, 20, 3, 20);
                line(3, 20, 3, 4); line(3, 10, 21, 10); line(3, 15, 21, 15);
                line(9, 4, 9, 20); line(15, 4, 15, 20); break;
            case UiIcon::Image:
                stroke(3,4,21,4,1.7f); stroke(21,4,21,20,1.7f);
                stroke(21,20,3,20,1.7f); stroke(3,20,3,4,1.7f);
                stroke(4,18,10,11,1.7f); stroke(10,11,15,16,1.7f); stroke(15,16,18,13,1.7f);
                stroke(18,13,20,16,1.7f); dot("sun", 16.5f, 8.0f, 3.4f); break;
            case UiIcon::Code:
                line(7, 6, 2, 12); line(2, 12, 7, 18); line(17, 6, 22, 12);
                line(22, 12, 17, 18); line(14, 3, 10, 21); break;
            case UiIcon::Typewriter:
                line(3, 4, 21, 4); line(3, 20, 21, 20); line(4, 12, 20, 12);
                line(1, 9, 4, 12); line(4, 12, 1, 15); line(23, 9, 20, 12);
                line(20, 12, 23, 15); break;
            case UiIcon::Focus:
                line(3, 9, 3, 3); line(3, 3, 9, 3); line(15, 3, 21, 3);
                line(21, 3, 21, 9); line(21, 15, 21, 21); line(21, 21, 15, 21);
                line(9, 21, 3, 21); line(3, 21, 3, 15); break;
            case UiIcon::Maximize:
                line(5, 5, 19, 5); line(19, 5, 19, 19); line(19, 19, 5, 19); line(5, 19, 5, 5); break;
            case UiIcon::Restore:
                line(8, 4, 20, 4); line(20, 4, 20, 16); line(5, 8, 16, 8);
                line(16, 8, 16, 20); line(16, 20, 5, 20); line(5, 20, 5, 8); break;
            case UiIcon::None: break;
            case UiIcon::Search:
                ui.rect(id + ".ring").position(4*scale, 4*scale).size(12*scale, 12*scale)
                    .radius(6*scale).color(rgba(0, 0, 0, 0)).border(1.7f*scale, color).build();
                line(15, 15, 21, 21);
                break;
            case UiIcon::Outline:
                line(4, 5, 7, 5); line(10, 5, 21, 5);
                line(7, 12, 10, 12); line(13, 12, 21, 12);
                line(10, 19, 13, 19); line(16, 19, 21, 19);
                break;
            case UiIcon::File:
            case UiIcon::TextFile:
            case UiIcon::MarkdownFile:
            case UiIcon::CodeFile:
            case UiIcon::DataFile:
            case UiIcon::ImageFile: document(icon); break;
            case UiIcon::ChevronRight: line(9, 6, 15, 12); line(15, 12, 9, 18); break;
            case UiIcon::ChevronDown: line(6, 9, 12, 15); line(12, 15, 18, 9); break;
            case UiIcon::Previous:
                line(12, 20, 12, 4); line(12, 4, 6, 10); line(12, 4, 18, 10); break;
            case UiIcon::Next:
                line(12, 4, 12, 20); line(12, 20, 6, 14); line(12, 20, 18, 14); break;
            case UiIcon::Close: line(6, 6, 18, 18); line(18, 6, 6, 18); break;
            case UiIcon::ArrowLeft:
                line(20, 12, 4, 12); line(4, 12, 10, 6); line(4, 12, 10, 18); break;
            case UiIcon::ArrowRight:
                line(4, 12, 20, 12); line(20, 12, 14, 6); line(20, 12, 14, 18); break;
            case UiIcon::Replace:
                line(4, 7, 19, 7); line(19, 7, 15, 3); line(19, 7, 15, 11);
                line(20, 17, 5, 17); line(5, 17, 9, 13); line(5, 17, 9, 21); break;
            case UiIcon::Folder:
                line(3, 6, 10, 6); line(10, 6, 12, 9); line(12, 9, 21, 9);
                line(21, 9, 21, 20); line(21, 20, 3, 20); line(3, 20, 3, 6); break;
            case UiIcon::FolderOpen:
                line(3, 6, 10, 6); line(10, 6, 12, 9); line(12, 9, 21, 9);
                line(21, 9, 18, 19); line(18, 19, 3, 19); line(3, 19, 3, 6);
                line(4, 11, 20, 11); break;
            case UiIcon::Globe:
                ui.rect(id + ".globe.ring").position(3*scale, 3*scale).size(18*scale, 18*scale)
                    .radius(9*scale).color(rgba(0, 0, 0, 0)).border(1.45f*scale, color).build();
                line(12, 3.5f, 9, 7); line(9, 7, 8, 12); line(8, 12, 9, 17);
                line(9, 17, 12, 20.5f); line(12, 3.5f, 15, 7); line(15, 7, 16, 12);
                line(16, 12, 15, 17); line(15, 17, 12, 20.5f);
                line(4, 9, 20, 9); line(3.5f, 12, 20.5f, 12); line(4, 15, 20, 15); break;
            case UiIcon::List:
                for (float y : {6.0f, 12.0f, 18.0f}) {
                    dot("list.dot." + std::to_string(segment++), 5, y, 1.8f);
                    line(9, y, 20, y);
                }
                break;
            case UiIcon::Font:
                line(4, 20, 10, 4); line(10, 4, 16, 20);
                line(6, 14, 14, 14); line(16, 9, 20, 9); line(18, 9, 18, 17);
                line(16, 13, 20, 13); line(16, 17, 20, 17); break;
            case UiIcon::Settings: {
                // Win11 记事本那颗齿轮：八齿外轮廓描边 + 中间圆孔。齿顶与齿间缺口
                // 各取一段角度，齿顶是短直边（8.6 半径上 15° 的弦只有 2.2 单位，
                // 再细分看不出差别），齿间缺口用一段弦带过。
                // 用户反馈原稿"太粗太厚重"：齿浅一档（root 6.8→7.1）、
                // 笔画细一档（1.65→1.4，孔同），视觉重量向侧栏其它线性图标看齐。
                constexpr int kTeeth = 8;
                constexpr float kOuter = 8.6f;
                constexpr float kRoot = 7.1f;
                constexpr float kPitch = 6.28318531f / kTeeth;
                constexpr float kRootOffset = 0.192f;  // 齿间缺口半宽 ≈ 11°
                constexpr float kTipOffset = 0.131f;   // 齿顶半宽 ≈ 7.5°
                const auto polar = [](float radius, float angle) {
                    return core::Vec2{12.0f + radius * std::cos(angle),
                                      12.0f + radius * std::sin(angle)};
                };
                std::vector<core::Vec2> contour;
                contour.reserve(kTeeth * 4);
                for (int index = 0; index < kTeeth; ++index) {
                    const float center = kPitch * static_cast<float>(index);
                    contour.push_back(polar(kRoot, center - kRootOffset));
                    contour.push_back(polar(kOuter, center - kTipOffset));
                    contour.push_back(polar(kOuter, center + kTipOffset));
                    contour.push_back(polar(kRoot, center + kRootOffset));
                }
                for (std::size_t index = 0; index < contour.size(); ++index) {
                    const core::Vec2& from = contour[index];
                    const core::Vec2& to = contour[(index + 1) % contour.size()];
                    stroke(from.x, from.y, to.x, to.y, 1.4f);
                }
                ui.rect(id + ".settings.hole").position(9.25f*scale, 9.25f*scale)
                    .size(5.5f*scale, 5.5f*scale)
                    .radius(2.75f*scale).color(rgba(0, 0, 0, 0)).border(1.4f*scale, color).build();
                break;
            }
            case UiIcon::Plus: line(12, 4, 12, 20); line(4, 12, 20, 12); break;
            case UiIcon::Minus: line(4, 12, 20, 12); break;
            case UiIcon::Check:
                line(4, 12.5f, 9.5f, 18); line(9.5f, 18, 20, 6); break;
            case UiIcon::Info:
                ui.rect(id + ".info.ring").position(3*scale, 3*scale).size(18*scale, 18*scale)
                    .radius(9*scale).color(rgba(0, 0, 0, 0)).border(1.5f*scale, color).build();
                dot("info.dot", 12, 7.5f, 1.8f); line(12, 11, 12, 17); break;
            case UiIcon::Warning:
                line(12, 3, 21, 20); line(21, 20, 3, 20); line(3, 20, 12, 3);
                line(12, 9, 12, 14); dot("warning.dot", 12, 17, 1.5f); break;
            case UiIcon::Sliders:
                line(4, 6, 20, 6); line(4, 12, 20, 12); line(4, 18, 20, 18);
                dot("slider.knob.0", 9, 6, 4); dot("slider.knob.1", 15, 12, 4);
                dot("slider.knob.2", 7, 18, 4); break;
        }
    }).build();
}

inline void iconTextButton(eui::Ui& ui, const std::string& id, UiIcon icon,
                           const std::string& label, float width, float fontSize,
                           std::function<void()> onClick, const char* fontFamily = kUiFontFamily) {
    const auto& colors = editorColors();
    const float height = fontSize + 14.0f;
    const float glyph = std::min(18.0f, fontSize + 2.0f);
    ui.stack(id).size(width, height).content([&] {
        ui.rect(id + ".hit").fill().radius(neo::corner(colors, 5.0f)).states(rgba(0, 0, 0, 0), colors.rowHover, colors.pressed)
            .cursor(eui::CursorShape::Hand).preserveFocusOnPress().onClick(std::move(onClick)).build();
        iconView(ui, id + ".icon", icon, 6, (height-glyph)*.5f, glyph, colors.text);
        ui.text(id + ".text").position(glyph+12, 0).size(std::max(0.0f, width-glyph-18), height)
            .text(label).fontFamily(fontFamily).fontSize(fontSize).color(colors.text)
            .verticalAlign(eui::VerticalAlign::Center).build();
    }).build();
}

inline void iconButton(eui::Ui& ui, const std::string& id, UiIcon icon,
                       float x, float y, float size, bool enabled,
                       std::function<void()> onClick) {
    const auto& colors = editorColors();
    ui.stack(id).position(x, y).size(size, size).content([&] {
        auto hit = ui.rect(id + ".hit").fill().radius(neo::corner(colors, 5.0f))
            .states(rgba(0, 0, 0, 0), colors.rowHover, colors.pressed)
            .preserveFocusOnPress().disabled(!enabled);
        if (enabled) hit.cursor(eui::CursorShape::Hand).onClick(std::move(onClick));
        hit.build();
        const float glyph = std::min(18.0f, size - 10.0f);
        iconView(ui, id + ".glyph", icon, (size-glyph)*.5f, (size-glyph)*.5f,
                 glyph, enabled ? colors.text : colors.textMuted);
    }).build();
}

} // namespace neo
