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
            geometry.stacked=stacked;geometry.labelWidth=width;
            geometry=neo::settings_detail::settingsRowGeometry(geometry,metrics,neo::kUiFontFamily,"Ag中文",
                "Ag中文 Ag中文 Ag中文 Ag中文 Ag中文 Ag中文 Ag中文");
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
            const float textArea=geometry.cardHeight-(stacked?56.0f:0.0f);
            const float expected=(20+textArea*.5f)*scale;
            if(std::fabs((backend.top+backend.bottom)*.5f-expected)>=.8f)
                std::cerr<<"ink observation scale="<<scale<<" font="<<size<<" width="<<width
                    <<" stacked="<<stacked<<" top="<<backend.top<<" bottom="<<backend.bottom
                    <<" expected center="<<expected<<'\n';
            check(std::fabs((backend.top+backend.bottom)*.5f-expected)<.8f,
                  "actual wrapped settings ink block is vertically centered");
        }
    core::TextPrimitive::setLayoutPixelScale(1);
}

void settingsRowsAndPreviewsFit() {
    using namespace neo::settings_detail;
    for (const char* language : {"zh-CN", "en"}) {
        neo::i18n::setPreference(language);
        for (float font : {12.0f, 18.0f}) for (float width : {284.0f, 368.0f, 848.0f}) {
            neo::AppState state; state.uiFontSize=font;
            const auto metrics=neo::uiMetrics(state);
            PanelGeometry base; base.contentWidth=width;
            base.stacked=width<700; base.labelWidth=base.stacked?width-36:width-330;
            const auto shortRow=settingsRowGeometry(base,metrics,neo::uiFontFamily(state),
                neo::i18n::tr("settings.line_numbers"),neo::i18n::tr("settings.line_numbers_hint"));
            const auto longRow=settingsRowGeometry(base,metrics,neo::uiFontFamily(state),
                neo::i18n::tr("settings.body_font"),std::string(15,'W')+" / Long font family, Regular / 中文字体名称");
            check(longRow.cardHeight>=shortRow.cardHeight,"long hints grow only their own setting row");
            check(shortRow.controlTop>=0 && shortRow.controlTop+34<=shortRow.cardHeight,
                  "row controls stay inside the independently measured card");
            for(float body : {12.0f,16.0f,32.0f}) {
                state.editorFontSize=body;
                const auto revision=state.revision;const auto text=state.doc.text;
                const auto style=settingsPreviewStyle(state);
                check(style.bodySize==body,"preview uses actual body size rather than interface size");
                for(bool editor : {false,true}) {
                    core::dsl::Ui ui;ui.begin("settings-preview-test");
                    const float measured=settingsSizePreview(nullptr,state,editor,base,0);
                    const float drawn=settingsSizePreview(&ui,state,editor,base,0);
                    ui.end();ui.layout(core::dsl::Screen{1000,2000});
                    const std::string id=editor?"settings.preview.editor":"settings.preview.ui";
                    const auto* card=ui.find(id);const auto* label=ui.find(id+".label");
                    const auto* sample=ui.find(id+(editor?".document":".sidebar"));
                    check(measured==drawn && card && label && sample,"measured previews build the same complete sample");
                    if(card && label && sample) {
                        check(sample->frame.y>=label->frame.y+label->frame.height &&
                              sample->frame.y+sample->frame.height<=card->frame.y+card->frame.height-10,
                              "wrapped preview content and labels fit their measured card at maximum size");
                    }
                }
                check(state.revision==revision && state.doc.text==text,"size previews do not modify the open document");
            }
        }
    }
}

void fontSearchMatchesNamesAndFiles() {
    neo::fonts::Entry font;
    font.displayName="Cascadia Code 中文";font.path="C:/Windows/Fonts/CascadiaCode-Regular.ttf";
    check(neo::fonts::matchesSearch(font,"CAScadia code"),"font search ignores English case");
    check(neo::fonts::matchesSearch(font,"中文"),"font search preserves Chinese UTF-8 names");
    check(neo::fonts::matchesSearch(font,"regular.TTF"),"font search includes the file basename");
    check(neo::fonts::matchesSearch(font,"  Code\tRegular  "),"search words may match name and filename together");
    check(neo::fonts::matchesSearch(font," \t "),"empty search restores the complete category list");
    check(!neo::fonts::matchesSearch(font,"Arial"),"unmatched font names are filtered out");
    check(!neo::fonts::matchesSearch(font,"Windows"),"search does not match unrelated directory names");
    font.path="D:\\fonts\\自定义字体.otf";
    check(neo::fonts::matchesSearch(font,"自定义字体"),"imported font filenames remain searchable");
    for(float size : {12.0f,18.0f}) {
        neo::AppState state;state.uiFontSize=size;
        const auto layout=neo::settings_detail::fontPickerSearchLayout(neo::uiMetrics(state));
        check(layout.countY>=layout.searchY+layout.searchHeight &&
              layout.sampleY>=layout.countY+layout.countHeight,
              "search, result count and sample occupy separate measured rows");
        check(layout.minimumHeight-54-layout.sampleY-12>=layout.rowHeight*3-.01f,
              "short font pages retain a reachable three-row list and footer");
    }
}

} // namespace

int main() {
    settingsInkBlocksAreCentered();
    settingsRowsAndPreviewsFit();
    fontSearchMatchesNamesAndFiles();
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
