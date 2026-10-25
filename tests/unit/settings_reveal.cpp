#include "../../apps/neo_editor/ui/interaction_reveal.h"
#include "../../apps/neo_editor/ui/settings_panel.h"
#include "../../apps/neo_editor/ui/widgets.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <tuple>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

bool approximatelyEqual(float a, float b) { return std::fabs(a - b) < 0.001f; }

struct InkRecorder final : core::render::RenderBackend {
    float top = std::numeric_limits<float>::infinity(), bottom = -top;
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int,int) override { return false; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int,int,const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int,int,core::render::RenderCacheBlitMode,const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool,const core::Rect&,int) override {}
    void prepareBackdropBlur(const core::Rect&,float,int,int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&,int,int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&,int,int) override {}
    void drawText(const core::render::TextDrawCommand& c,int,int) override {
        for(std::size_t i=1;i<c.vertexFloatCount;i+=5) {
            top=std::min(top,c.vertices[i]);bottom=std::max(bottom,c.vertices[i]);
        }
    }
};

void settingsInkBlocksAreCentered() {
    InkRecorder backend;core::render::ScopedRenderBackend scoped(backend);
    for(float scale:{1.25f,1.5f}) for(float size:{12.0f,14.0f,18.0f})
        for(float width:{140.0f,260.0f}) for(bool stacked:{false,true}) {
            core::TextPrimitive::setLayoutPixelScale(scale);
            core::dsl::Ui ui;ui.begin("settings-ink");
            neo::UiMetrics metrics;metrics.panelLabelFontSize=size;metrics.panelHintFontSize=size-1;
            neo::EditorColors colors{};colors.tokens=components::theme::light();
            neo::settings_detail::PanelGeometry geometry;geometry.contentWidth=width+36;
            geometry.stacked=stacked;geometry.cardHeight=160+(stacked?48:0);
            neo::settings_detail::rowLabel(ui,colors,metrics,neo::kUiFontFamily,"row","Ag中文",
                "Ag中文 Ag中文 Ag中文 Ag中文 Ag中文 Ag中文 Ag中文",0,20,width,geometry);
            ui.end();ui.layout(core::dsl::Screen{600,600});
            backend.top=std::numeric_limits<float>::infinity();backend.bottom=-backend.top;
            for(const char* id:{"row.label","row.hint"}) {
                const auto* e=ui.find(id);check(e!=nullptr,"settings text block exists");
                if(!e) continue;
                core::TextPrimitive text;check(text.initialize(),"settings glyph renderer initializes");
                text.setText(e->text);text.setFontFamily(e->fontFamily);text.setFontSize(e->fontSize*scale);
                text.setFontWeight(e->fontWeight);text.setLineHeight(e->lineHeight*scale);
                text.setPosition(e->frame.x*scale,e->frame.y*scale);text.setMaxWidth(e->frame.width*scale);
                text.setWrap(e->wrap);text.render(2048,1536);
            }
            const float expected=(20+80)*scale;
            if(std::fabs((backend.top+backend.bottom)*.5f-expected)>=.8f)
                std::cerr<<"ink observation scale="<<scale<<" font="<<size<<" width="<<width
                    <<" stacked="<<stacked<<" top="<<backend.top<<" bottom="<<backend.bottom
                    <<" expected center="<<expected<<'\n';
            check(std::fabs((backend.top+backend.bottom)*.5f-expected)<.8f,
                  "actual wrapped settings ink block is vertically centered");
        }
    core::TextPrimitive::setLayoutPixelScale(1);
}

} // namespace

int main() {
    settingsInkBlocksAreCentered();
    // Production text measurement controls both the navigation row count and
    // the theme-library scroll range; cover translations and font changes.
    for (const char* language : {"zh-CN", "en"}) {
        neo::i18n::setPreference(language);
        for (float size : {12.0f, 18.0f}) {
            neo::AppState state; state.uiFontSize = size;
            const auto metrics = neo::uiMetrics(state);
            for (float width : {336.0f, 420.0f, 608.0f, 800.0f}) {
                const int columns = neo::settings_detail::settingsNavigationColumns(state,width);
                const float slotWidth = (width-24.0f)/columns-4.0f;
                for (const char* id : {"settings.appearance", "settings.editor", "settings.files_system", "settings.about_nav"})
                    check(core::TextPrimitive::measureTextWidth(neo::i18n::tr(id),neo::uiFontFamily(state),metrics.panelFontSize)
                          <= slotWidth-24.0f+.01f,"narrow navigation labels fit their measured slots");
                for (float height : {120.0f, 200.0f, 500.0f}) {
                    const float contentWidth=width-neo::settings_detail::kPadding*2;
                    const auto layout=neo::settings_detail::themeListLayout(metrics,neo::uiFontFamily(state),contentWidth,height);
                    core::TextStyle hint; hint.text=neo::i18n::tr("settings.theme_library_hint");
                    hint.fontFamily=neo::uiFontFamily(state);hint.fontSize=metrics.panelHintFontSize;
                    hint.lineHeight=metrics.panelHintFontSize+5;hint.wrap=true;hint.maxWidth=contentWidth;
                    check(core::TextPrimitive::measureTextSize(hint).y<=layout.hintHeight+.01f,
                          "theme-library hint retains every wrapped line");
                    check(layout.listY>=64+layout.hintHeight+12-.01f &&
                          layout.contentHeight-86-55-layout.listY>=64-.01f,
                          "wrapped hint and usable theme list never overlap actions");
                    check(layout.contentHeight-86+8+30-(layout.contentHeight-height)<=height,
                          "short theme page can scroll its footer fully into view");
                }
            }
        }
    }
    neo::i18n::setPreference("zh-CN");
    {
        neo::AppState state;
        state.openMenu=neo::MenuKind::View;state.contextMenuOpen=state.vaultContextMenuOpen=true;
        state.findOptionsOpen=state.markdownHeadingMenuOpen=state.languageMenuOpen=state.tabListOpen=true;
        const auto tab=state.tabId;const auto revision=state.revision;const auto text=state.doc.text;
        neo::popup_presentation::setSettingsOpen(state,true);
        check(state.settingsOpen && neo::popup_presentation::activeOwner(state)==neo::popup_presentation::Owner::None &&
              !state.tabListOpen,"settings entry clears every popup owner");
        neo::popup_presentation::setSettingsOpen(state,false);
        check(!state.settingsOpen && !state.contextMenuOpen && state.findEditorFocusPending,
              "settings round trip cannot resurrect the context menu");
        check(state.tabId==tab && state.revision==revision && state.doc.text==text,
              "page navigation preserves document identity and text");
    }
    using namespace neo::interaction_reveal;
    check(enabled(neo::UiStyle::Square, true, nullptr), "square enabled");
    check(!enabled(neo::UiStyle::Rounded, true, nullptr), "rounded disabled");
    check(!enabled(neo::UiStyle::Square, false, nullptr), "feedback disabled");
    check(!enabled(neo::UiStyle::Square, true, "0"), "environment override");

    // The reset control is the final row on both settings pages. Appearance has
    // eight regular rows (including interface style); Editor has six.
    constexpr float sectionHeight = 48.0f;
    constexpr float rowHeight = 92.0f;
    for (const auto [category, regularRows, label] : {
             std::tuple{neo::SettingsCategory::Appearance, 8, "appearance"},
             std::tuple{neo::SettingsCategory::Editor, 6, "editor"}}) {
        check(neo::settings_detail::settingsRowsBeforeReset(category) == regularRows,
              label);
        const float contentHeight = neo::settings_detail::settingsMainContentHeight(
            category, sectionHeight, rowHeight);
        const float resetBottom = sectionHeight + (regularRows + 1) * rowHeight;
        const float viewportHeight = 420.0f;
        const float maxOffset = std::max(0.0f, contentHeight - viewportHeight);
        check(resetBottom - maxOffset <= viewportHeight,
              "reset row remains reachable at maximum scroll");
    }

    neo::EditorColors colors{};
    colors.accent = {0.2f, 0.4f, 0.8f, 0.5f};
    const auto first = effect(colors, true);
    check(first.enabled && approximatelyEqual(first.tint.r, 0.2f) && approximatelyEqual(first.tint.a, 0.5f),
          "theme accent including alpha");
    colors.accent = {0.8f, 0.3f, 0.1f, 1.0f};
    const auto second = effect(colors, false);
    check(!second.enabled && approximatelyEqual(second.tint.r, 0.8f), "theme change and disable");
    check(first.borderWidth == 1.0f && first.radius == 72.0f &&
          first.fillOpacity < first.borderOpacity, "restrained light geometry");

    // Exercise the same final compose pass used by the application, including
    // inherited disable/hit-test barriers and round-trip style changes.
    core::dsl::Ui ui;
    ui.begin("reveal-policy");
    const auto action = [&](const char* id) {
        ui.rect(id).size(80,30).states({}, {}, {}).onClick([]{}).build();
    };
    action("toolbar.action");
    ui.rect("menu.item.0").size(120,28).cursor(core::CursorShape::Hand).onClick([]{}).build();
    ui.rect("shield").size(800,600).onClick([]{}).build();
    ui.rect("drag").size(800,600).states({}, {}, {}).onClick([]{})
        .onDrag([](const core::dsl::DragEvent&){}).build();
    ui.rect("input").size(200,32).states({}, {}, {}).onClick([]{})
        .onTextInput([](const core::TextInputEvent&){}).build();
    ui.stack("disabled").disabled().content([&]{action("disabled.action");}).build();
    ui.stack("hidden").hitTestMode(core::dsl::HitTestMode::None)
        .content([&]{action("hidden.action");}).build();
    action("window.close.bg");
    ui.stack("themeWorkbench.preview").content([&]{action("sample.action");}).build();
    neo::setUiStyle(neo::UiStyle::Square);
    neo::applyInteractionDefaults(ui, neo::kUiFontFamily, true);
    ui.end();
    check(ui.find("toolbar.action")->reveal && ui.find("menu.item.0")->reveal,
          "application pass enables buttons and menu rows");
    for (const char* id : {"shield", "drag", "input", "disabled.action", "hidden.action",
                           "window.close.bg", "sample.action"}) {
        check(!ui.find(id)->reveal, id);
    }
    check(ui.find("toolbar.action")->reveal->tint.r == neo::editorColors().accent.r,
          "application pass uses active theme accent");
    neo::applyInteractionDefaults(ui, neo::kUiFontFamily, false);
    check(!ui.find("toolbar.action")->reveal, "animation off removes existing effect");
    neo::applyInteractionDefaults(ui, neo::kUiFontFamily, true);
    check(ui.find("toolbar.action")->reveal != nullptr, "animation re-enable restores effect");
    neo::setUiStyle(neo::UiStyle::Rounded);
    neo::applyInteractionDefaults(ui, neo::kUiFontFamily, true);
    check(!ui.find("toolbar.action")->reveal, "rounded round-trip removes effect");

    if (failures == 0) std::cout << "settings_reveal: PASS\n";
    return failures == 0 ? 0 : 1;
}
