#pragma once

#include "model/i18n.h"

#include "model/lp_plan.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "components/virtuallist.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace neo {

// 只投影编辑器已经生成的 Live Preview 计划，不另跑一次 Markdown 解析。
// conceal 是同一计划里的可见性信息；ATX 前缀、闭合 # 与行内标记都由它去除。
inline std::string outlineTitle(const std::string& text, const LpLine& line) {
    const int beg = std::clamp(line.srcBeg, 0, static_cast<int>(text.size()));
    const int end = std::clamp(line.srcEnd, beg, static_cast<int>(text.size()));
    std::string result;
    int cursor = beg;
    for (const LpRange& hidden : line.conceal) {
        const int hiddenBeg = std::clamp(hidden.beg, beg, end);
        const int hiddenEnd = std::clamp(hidden.end, hiddenBeg, end);
        if (hiddenBeg > cursor) {
            result.append(text, static_cast<std::size_t>(cursor),
                          static_cast<std::size_t>(hiddenBeg - cursor));
        }
        cursor = std::max(cursor, hiddenEnd);
    }
    if (cursor < end) {
        result.append(text, static_cast<std::size_t>(cursor),
                      static_cast<std::size_t>(end - cursor));
    }
    const auto nonSpace = [](unsigned char ch) { return !std::isspace(ch); };
    const auto first = std::find_if(result.begin(), result.end(), nonSpace);
    const auto last = std::find_if(result.rbegin(), result.rend(), nonSpace).base();
    if (first >= last) {
        return i18n::tr("outline.empty_heading");
    }
    return std::string(first, last);
}

inline std::vector<OutlineEntry> outlineFromPlan(const std::string& text, const LpPlan& plan) {
    std::vector<OutlineEntry> entries;
    for (const LpLine& line : plan.lines) {
        // setext 下划线也被标记为 Heading，但它不是第二个标题，且 sectionEndLine=-1。
        if (line.kind != LpKind::Heading || line.headingLevel < 1 ||
            line.headingLevel > 6 || line.sectionEndLine < 0) {
            continue;
        }
        entries.push_back({outlineTitle(text, line), line.headingLevel, line.srcBeg, line.number});
    }
    return entries;
}

inline bool refreshVisibleOutline(AppState& state, const std::string& text,
                                  const LpPlan& plan, unsigned long long version) {
    if (!state.markdownOutlineOpen || state.markdownImmersive || !state.markdownCapable() ||
        state.outlinePlanVersion == version || text != state.doc.text) return false;
    state.outline = outlineFromPlan(text, plan);
    state.outlinePlanVersion = version;
    return true;
}

inline std::string vaultTabLabelForWidth(const std::string& text, float width,
                                         const char* fontFamily, float fontSize) {
    if (width <= 0.0f || text.empty()) return {};
    const auto measure = [&](const std::string& value) {
        return core::TextPrimitive::measureTextWidth(value, fontFamily, fontSize);
    };
    if (measure(text) <= width) return text;
    const std::string ellipsis = "\xE2\x80\xA6";
    if (measure(ellipsis) > width) return {};
    std::string prefix;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        const std::size_t length = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2
            : (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;
        const std::size_t next = std::min(text.size(), index + length);
        const std::string candidate = prefix + text.substr(index, next - index) + ellipsis;
        if (measure(candidate) > width) return prefix + ellipsis;
        prefix.append(text, index, next - index);
        index = next;
    }
    return text;
}

// Right-hand document outline. Its rows reuse the editor plan and source offsets.
inline void outlinePanelView(eui::Ui& ui, AppState& state, float width, float height, bool floating = false) {
    const auto& c = editorColors();
    const auto m = uiMetrics(state);
    const float inner = std::max(0.0f, width-32);
    const float cardHeight = std::min(std::max(0.0f,height-32),
        std::max(150.0f,70.0f+static_cast<float>(state.outline.size())*36));
    ui.stack("outline.panel").size(width, height).content([&] {
        if (!floating) ui.rect("outline.paper").fill().color(c.editor).build();
        ui.rect("outline.surface").position(8, 16).size(width-16, cardHeight)
            .radius(10).color(c.editor).border(1,c.border)
            .shadow(12,0,3,rgba(0,0,0,.06f)).build();
        ui.text("outline.title").position(20, 22).size(inner-30, 32).text(i18n::tr("outline.title"))
            .fontFamily(uiFontFamily(state)).fontSize(14).fontWeight(600).color(c.text)
            .verticalAlign(eui::VerticalAlign::Center).build();
        iconButton(ui, "outline.collapse", UiIcon::ChevronRight, width-48, 22, 28, true, [&state] {
            state.markdownOutlineOpen=false; app::requestUpdate();
        });
        if (state.outline.empty()) {
            ui.text("outline.empty").position(20, 65).size(inner, 70).text(i18n::tr("outline.no_headings"))
                .fontFamily(uiFontFamily(state)).fontSize(13).color(c.textMuted).wrap(true).build();
            return;
        }
        components::virtualList(ui,"outline.list").position(16,60).size(inner,std::max(0.0f,cardHeight-54))
            .theme(c.tokens).itemCount(static_cast<std::int64_t>(state.outline.size()))
            .rowHeight(36).offset(state.outlineScroll).step(78).overscanViewports(.5f)
            .onChange([&state](float v){state.outlineScroll=v;})
            .row([&state,&c,&m,floating](eui::Ui& rowUi,const std::string& id,std::int64_t index,float w,float){
                if (index<0 || static_cast<std::size_t>(index)>=state.outline.size()) return;
                const auto entry=state.outline[static_cast<std::size_t>(index)];
                const bool active=entry.byteOffset==state.outlineCurrentByte;
                const float indent=10.0f+(entry.level-1)*12.0f;
                rowUi.stack(id).size(w,36).content([&, floating]{
                    rowUi.rect(id+".bg").position(0,2).size(w,32).radius(5)
                        .states(active?core::mixColor(c.editor,c.accent,.10f):transparentColor(),c.rowHover,c.pressed)
                        .preserveFocusOnPress().cursor(eui::CursorShape::Hand).onClick([&state,entry,floating]{
                            state.cancelLinkAnchor();state.pendingOutlineJumpByte=entry.byteOffset;
                            state.findEditorFocusPending=true;
                            if(floating) state.markdownOutlineOpen=false;
                            app::requestUpdate();
                        }).build();
                    rowUi.rect(id+".level.bg").position(indent,9).size(25,18).radius(4)
                        .color(core::mixColor(c.editor,c.accent,active?.16f:.07f)).build();
                    rowUi.text(id+".level").position(indent,9).size(25,18)
                        .text("H"+std::to_string(entry.level)).fontSize(10).fontWeight(600)
                        .color(active?c.accent:c.textMuted)
                        .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                    rowUi.text(id+".label").position(indent+32,0).size(std::max(0.0f,w-indent-40),36)
                        .text(elideToWidth(entry.title,w-indent-40,m.vaultRowFontSize))
                        .fontFamily(uiFontFamily(state)).fontSize(m.vaultRowFontSize)
                        .fontWeight(active?600:400).color(active?c.accent:c.textMuted)
                        .verticalAlign(eui::VerticalAlign::Center).build();
                }).build();
            }).build();
    }).build();
}

} // namespace neo
