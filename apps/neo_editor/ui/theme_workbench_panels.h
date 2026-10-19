#pragma once
#include "ui/theme_workbench_actions.h"
#include "ui/theme_workbench_widgets.h"
#include "components/input.h"
#include "components/scrollview.h"

namespace neo::theme_workbench_ui {
inline void picker(eui::Ui& ui,const std::string& id,float x,float y,float w,const EditorColors& c,const Dim& d,bool seed) {
    auto& s=session();const float values[]{s.pickH,s.pickS,s.pickV,s.pickA};
    constexpr const char* names[]{"theme_wb.hue","theme_wb.saturation","theme_wb.brightness","theme_wb.opacity"};
    for(int channel=0;channel<(seed?3:4);++channel){
        const float yy=y+channel*(d.row+6),trackY=yy+d.hint+5,th=std::max(14.0f,d.control*.44f);
        const std::string key=id+"."+std::to_string(channel);
        label(ui,key+".label",tr(names[channel]),x,yy,w*.65f,d.hint+4,c.textMuted,d.hint);
        label(ui,key+".value",std::to_string(static_cast<int>(std::lround(values[channel]*(channel==0?360:100)))),x+w*.65f,yy,w*.35f,d.hint+4,c.text,d.hint,400,eui::HorizontalAlign::Right);
        for(int cell=0;cell<24;++cell){
            const float t=static_cast<float>(cell)/23;
            auto col=fromHsv(channel==0?t:s.pickH,channel==1?t:s.pickS,channel==2?t:s.pickV);
            if(channel==0)col=fromHsv(t,1,1);if(channel==3)col=core::mixColor(c.editor,col,t);
            ui.rect(key+".cell."+std::to_string(cell)).position(x+w*cell/24,trackY).size(w/24+.5f,th).color(col).build();
        }
        ui.rect(key+".knob").position(x+std::clamp(values[channel]*w,2.0f,w-3)-2,trackY-2).size(4,th+4).radius(2).color(c.text).border(1,c.editor).build();
        if(seed||!readOnly())ui.rect(key+".hit").position(x,trackY-4).size(w,th+8).color(transparentColor())
            .cursor(eui::CursorShape::Hand).preserveFocusOnPress()
            .onPress([channel,seed](const core::PointerEvent& e,const core::Rect& r){
                if(e.button!=core::PointerButton::Left)return;auto& a=session();a.dragging=true;a.dragRecorded=false;a.dragRect=r;
                changePicker(static_cast<float>((e.x-r.x)/std::max(1.0f,r.width)),channel,seed);
            })
            .onDrag([channel,seed](const core::dsl::DragEvent& e){
                if(e.button!=core::PointerButton::Left||!session().dragging)return;const auto r=session().dragRect;
                changePicker(static_cast<float>((e.x-r.x)/std::max(1.0f,r.width)),channel,seed);
            })
            .onRelease([](const core::PointerEvent&,const core::Rect&){session().dragging=false;session().dragRecorded=false;}).build();
    }
}

inline void colorLayer(eui::Ui& ui,float w,const EditorColors& c,const Dim& d) {
    auto& s=session();const float pad=d.pad,inner=std::max(80.0f,w-pad*2);
    ui.stack("themeWorkbench.color.content").size(w,d.row*12+d.control*3+150).content([&]{
        float y=0;
        ghostButton(ui,"themeWorkbench.color.back",tr("theme_wb.back_palette"),pad,y,inner,d.control,c,d.font,[]{session().layer=Layer::Palette;session().panelScroll=0;app::requestUpdate();},false,false,UiIcon::ArrowLeft);y+=d.control+d.gap;
        label(ui,"themeWorkbench.color.title",fieldLabel(s.selected),pad,y,inner,d.row,c.text,d.title,600);y+=d.row;
        paragraph(ui,"themeWorkbench.color.affects",fieldAffects(s.selected),pad,y,inner,d.hint*3.2f,c.textMuted,d.hint);y+=d.hint*3.2f+d.gap;
        const auto* f=findThemeColorField(s.selected);const auto before=f?themeworkbench::colors(draft(),s.light,true).*(f->member):rgba(0,0,0);
        swatch(ui,"themeWorkbench.color.before",pad,y,42,d.control,before,c);
        swatch(ui,"themeWorkbench.color.current",pad+50,y,42,d.control,selectedColor(),c);
        label(ui,"themeWorkbench.color.side",tr(s.light?"theme_wb.light":"theme_wb.dark"),pad+105,y,inner-105,d.control,c.textMuted,d.hint);y+=d.control+d.gap;
        label(ui,"themeWorkbench.color.baseline",std::string(tr("theme_wb.original"))+" "+hexColor(before,true),pad,y,inner,d.row,c.textMuted,d.hint);y+=d.row;
        std::string origin=tr("theme_wb.builtin");
        for(const auto& e:draft().baseline.importReport)if(e.light==s.light&&e.field==s.selected){origin=e.imported?e.source:e.reason;break;}
        paragraph(ui,"themeWorkbench.color.origin",std::string(tr("theme_wb.origin"))+" "+origin,pad,y,inner,d.row*1.5f,c.textMuted,d.hint);y+=d.row*1.5f+d.gap;
        if(readOnly()){paragraph(ui,"themeWorkbench.color.readonly",tr("theme_wb.readonly"),pad,y,inner,d.row*2,c.textMuted,d.font);return;}
        components::input(ui,"themeWorkbench.color.hex").position(pad,y).size(inner*.62f,d.control).value(s.hexDraft).placeholder("#RRGGBBAA").fontSize(d.font).theme(c.tokens)
            .onChange([](const std::string& v){
                auto& a=session();a.hexDraft=v;auto col=selectedColor();
                if(!readOnly()&&parseHex(v,col)){themeworkbench::setColor(draft(),a.light,a.selected,col);const auto h=toHsv(col);a.pickH=h.h;a.pickS=h.s;a.pickV=h.v;a.pickA=col.a;a.alphaDraft=std::to_string(static_cast<int>(std::lround(col.a*100)));app::requestUpdate();}
            }).build();
        components::input(ui,"themeWorkbench.color.alpha").position(pad+inner*.67f,y).size(inner*.33f,d.control).value(s.alphaDraft).placeholder("%").fontSize(d.font).theme(c.tokens)
            .onChange([](const std::string& v){
                auto& a=session();a.alphaDraft=v;try{std::size_t n=0;const int p=std::stoi(v,&n);if(n!=v.size()||p<0||p>100||readOnly())return;
                    auto col=selectedColor();col.a=p/100.0f;themeworkbench::setColor(draft(),a.light,a.selected,col);a.pickA=col.a;a.hexDraft=hexColor(col,true);app::requestUpdate();}catch(...){}
            }).build();y+=d.control+d.gap*2;
        picker(ui,"themeWorkbench.color.picker",pad,y,inner,c,d,false);y+=(d.row+6)*4+d.gap;
        toggle(ui,"themeWorkbench.color.lock",pad,y,inner,d.row,c,d.font,tr("theme_wb.lock"),themeworkbench::isLocked(draft(),s.light,s.selected),[](bool value){if(!readOnly()){themeworkbench::setLocked(draft(),session().light,session().selected,value);app::requestUpdate();}});y+=d.row+d.gap;
        ghostButton(ui,"themeWorkbench.color.restore",tr("theme_wb.restore_color"),pad,y,inner,d.control,c,d.font,[]{if(!readOnly()){themeworkbench::restoreColor(draft(),session().light,session().selected);syncInputs();app::requestUpdate();}});
    }).build();
}

inline void paletteLayer(eui::Ui& ui,AppState& state,float w,const EditorColors& c,const Dim& d) {
    auto& s=session();const float pad=d.pad,inner=std::max(90.0f,w-pad*2);
    const int fields=s.expandedGroup<0?0:static_cast<int>(groups()[s.expandedGroup].size());
    const float contentH=400+d.row*(12+fields)+(s.seedEditing?(d.row+6)*3:0)+(s.draftMenu?d.row*s.drafts.size():0);
    ui.stack("themeWorkbench.palette.content").size(w,contentH).content([&]{
        float y=0;
        label(ui,"themeWorkbench.palette.title",tr("theme_wb.palette"),pad,y,inner,d.row,c.text,d.title,600);y+=d.row;
        paragraph(ui,"themeWorkbench.palette.help",tr("theme_wb.palette_help"),pad,y,inner,d.hint*3.2f,c.textMuted,d.hint);y+=d.hint*3.2f+d.gap;
        eui::Color seed;parseHex(s.seedHex,seed);swatch(ui,"themeWorkbench.seed.swatch",pad,y,d.control,d.control,seed,c);
        components::input(ui,"themeWorkbench.seed.hex").position(pad+d.control+d.gap,y).size(inner-d.control-d.gap,d.control).value(s.seedHex).placeholder("#RRGGBB").fontSize(d.font).theme(c.tokens)
            .onChange([](const std::string& v){session().seedHex=v;generateCandidates();}).build();y+=d.control+d.gap;
        ghostButton(ui,"themeWorkbench.seed.edit",tr("theme_wb.seed_picker"),pad,y,inner*.5f,d.control,c,d.font,[]{auto& a=session();a.seedEditing=!a.seedEditing;eui::Color col;parseHex(a.seedHex,col);const auto h=toHsv(col);a.pickH=h.h;a.pickS=h.s;a.pickV=h.v;a.pickA=1;app::requestUpdate();},s.seedEditing,s.compare);
        ghostButton(ui,"themeWorkbench.seed.shuffle",tr("theme_wb.shuffle"),pad+inner*.5f,y,inner*.5f,d.control,c,d.font,[]{session().autoRound=(session().autoRound+1)%7;generateCandidates();},false,s.compare);y+=d.control+d.gap;
        if(s.seedEditing){picker(ui,"themeWorkbench.seed.picker",pad,y,inner,c,d,true);y+=(d.row+6)*3+d.gap;}
        if(hasImportedColors()){toggle(ui,"themeWorkbench.palette.missing",pad,y,inner,d.row,c,d.hint,tr("theme_wb.keep_imported"),s.missingOnly,[](bool value){session().missingOnly=value;app::requestUpdate();});y+=d.row+d.gap;}
        for(int i=0;i<static_cast<int>(s.candidates.size());++i){
            const auto colors=themeworkbench::colors(candidateDraft(i),s.light);const std::string id="themeWorkbench.candidate."+std::to_string(i);
            ui.rect(id+".bg").position(pad,y).size(inner,d.row+10).radius(6).states(c.editor,c.rowHover,c.pressed)
                .border(s.trial==i?2.0f:1.0f,s.trial==i?c.accent:c.border).cursor(eui::CursorShape::Hand).preserveFocusOnPress()
                .onHover([i](bool inside){if(!session().compare){if(inside)session().trial=i;else if(session().trial==i)session().trial=-1;app::requestUpdate();}})
                .onClick([i]{if(!session().compare){themeworkbench::adoptGenerated(draft(),session().candidates[i],recipe(i),session().missingOnly);session().trial=-1;session().message=tr("theme_wb.adopted");syncInputs();app::requestUpdate();}}).build();
            const eui::Color samples[]{colors.editor,colors.panel,colors.heading,colors.accent};
            for(int j=0;j<4;++j)swatch(ui,id+".swatch."+std::to_string(j),pad+10+j*17,y+(d.row-12)*.5f,14,22,samples[j],c,3);
            label(ui,id+".text",std::string(variantLabel(i))+" · "+tr("theme_wb.adopt"),pad+88,y,inner-98,d.row+10,c.text,d.font);y+=d.row+18;
        }
        paragraph(ui,"themeWorkbench.palette.candidate_hint",tr("theme_wb.candidate_help"),pad,y,inner,d.row*1.5f,c.textMuted,d.hint);y+=d.row*1.5f+d.gap;
        hairline(ui,"themeWorkbench.palette.line",pad,y,inner,1,c);y+=d.gap;
        for(int g=0;g<4;++g){
            ghostButton(ui,"themeWorkbench.group."+std::to_string(g),groupLabel(g),pad,y,inner,d.row,c,d.font,[g]{session().expandedGroup=session().expandedGroup==g?-1:g;app::requestUpdate();},s.expandedGroup==g,false,s.expandedGroup==g?UiIcon::ChevronDown:UiIcon::ChevronRight);y+=d.row;
            if(s.expandedGroup==g)for(const auto& key:groups()[g]){
                const auto* f=findThemeColorField(key);const auto color=f?shownColors().*(f->member):rgba(0,0,0);
                ghostButton(ui,"themeWorkbench.field."+key,fieldLabel(key),pad+30,y,inner-30,d.row,c,d.font,[key]{select(key);});
                swatch(ui,"themeWorkbench.field.swatch."+key,pad+6,y+(d.row-18)*.5f,18,18,color,c,3);y+=d.row;
            }
        }
        y+=d.gap;
        ghostButton(ui,"themeWorkbench.type.open",tr("theme_wb.typography"),pad,y,inner,d.row,c,d.font,[]{session().layer=Layer::Type;session().typeScroll=0;session().trial=-1;session().seedEditing=false;app::requestUpdate();},false,false,UiIcon::Font);y+=d.row;
        ghostButton(ui,"themeWorkbench.restore_all",tr("theme_wb.restore_all"),pad,y,inner,d.row,c,d.font,[]{if(!readOnly()){themeworkbench::restoreAll(draft());syncInputs();app::requestUpdate();}},false,readOnly());y+=d.row;
        ghostButton(ui,"themeWorkbench.import",tr("theme_wb.import"),pad,y,inner,d.row,c,d.font,[&state]{chooseFile(state);});y+=d.row;
        ghostButton(ui,"themeWorkbench.drafts",tr("theme_wb.drafts"),pad,y,inner,d.row,c,d.font,[]{session().draftMenu=!session().draftMenu;app::requestUpdate();},s.draftMenu);y+=d.row;
        if(s.draftMenu)for(std::size_t i=0;i<s.drafts.size();++i){
            const auto name=s.drafts[i].current.name.empty()?std::string(tr("theme_wb.untitled")):s.drafts[i].current.name;
            ghostButton(ui,"themeWorkbench.draft."+std::to_string(i),name+(dirty(i)?" *":""),pad+12,y,inner-12,d.row,c,d.font,[i]{session().active=i;prepareDraft();app::requestUpdate();},s.active==i);y+=d.row;
        }
        ghostButton(ui,"themeWorkbench.new",tr("theme_wb.new_draft"),pad,y,inner,d.row,c,d.font,[]{auto& a=session();a.drafts.push_back(themeworkbench::makeDraft(ThemeFileData{}));a.active=a.drafts.size()-1;a.drafts.back().current.name=tr("theme_wb.untitled");prepareDraft();app::requestUpdate();},false,false,UiIcon::Plus);
    }).build();
}

struct TypeField {const char* id;std::optional<float> ThemeTypography::*member;float fallback,min,max;};
inline void changeType(TypeField f,float bodyPx,float delta) {
    auto& s=session();if(readOnly())return;
    auto t=(s.light?draft().current.light:draft().current.dark).typography;
    t.*(f.member)=std::clamp((t.*(f.member)).value_or(f.fallback)+delta/bodyPx,f.min,f.max);
    if(s.shareType)themeworkbench::setTypographyBoth(draft(),t);else themeworkbench::setTypography(draft(),s.light,t);
    app::requestUpdate();
}
inline void typeLayer(eui::Ui& ui,float w,const EditorColors& c,const Dim& d,float bodyPx) {
    auto& s=session();const float pad=d.pad,inner=std::max(90.0f,w-pad*2);
    const TypeField fields[]{
        {"theme_wb.type.line",&ThemeTypography::bodyLineHeight,kMarkdownBodyLineHeight,.5f,4},
        {"theme_wb.type.gap",&ThemeTypography::blockGap,kMarkdownBlockGap,0,4},
        {"theme_wb.type.h1",&ThemeTypography::h1SizeFactor,1.618f,.5f,4},
        {"theme_wb.type.h2",&ThemeTypography::h2SizeFactor,1.462f,.5f,4},
        {"theme_wb.type.h3",&ThemeTypography::h3SizeFactor,1.318f,.5f,4},
        {"theme_wb.type.indent",&ThemeTypography::listIndent,kMarkdownListIndent,0,8},
        {"theme_wb.type.code",&ThemeTypography::codeSizeFactor,kMarkdownCodeSizeFactor,.5f,2},
        {"theme_wb.type.radius",&ThemeTypography::radius,kMarkdownRadius,0,2},
    };
    ui.stack("themeWorkbench.type.content").size(w,d.row*14+d.control*10+d.gap*12).content([&]{
        float y=0;
        ghostButton(ui,"themeWorkbench.type.back",tr("theme_wb.back_palette"),pad,y,inner,d.control,c,d.font,[]{session().layer=Layer::Palette;session().panelScroll=0;app::requestUpdate();},false,false,UiIcon::ArrowLeft);y+=d.control+d.gap;
        label(ui,"themeWorkbench.type.title",tr("theme_wb.typography"),pad,y,inner,d.row,c.text,d.title,600);y+=d.row;
        paragraph(ui,"themeWorkbench.type.help",tr("theme_wb.type_help"),pad,y,inner,d.row*2,c.textMuted,d.hint);y+=d.row*2;
        toggle(ui,"themeWorkbench.type.share",pad,y,inner,d.row,c,d.hint,tr("theme_wb.share_type"),s.shareType,[](bool v){session().shareType=v;app::requestUpdate();});y+=d.row+d.gap;
        const auto& theme=s.compare?draft().baseline:draft().current;const auto t=(s.light?theme.light:theme.dark).typography;
        for(int i=0;i<8;++i){const auto f=fields[i];const float px=(t.*(f.member)).value_or(f.fallback)*bodyPx;const std::string id="themeWorkbench.type.field."+std::to_string(i);
            label(ui,id+".name",tr(f.id),pad,y,inner,d.row,c.text,d.font);y+=d.row;
            char value[32]{};std::snprintf(value,sizeof(value),"%.0f px",px);
            ghostButton(ui,id+".minus","−",pad,y,42,d.control,c,d.font,[f,bodyPx]{changeType(f,bodyPx,-1);},false,readOnly());
            label(ui,id+".value",value,pad+46,y,inner-92,d.control,c.text,d.font,400,eui::HorizontalAlign::Center);
            ghostButton(ui,id+".plus","+",pad+inner-42,y,42,d.control,c,d.font,[f,bodyPx]{changeType(f,bodyPx,1);},false,readOnly());y+=d.control+d.gap;
        }
        ghostButton(ui,"themeWorkbench.type.copy",tr("theme_wb.copy_type"),pad,y,inner,d.control,c,d.font,[]{auto& a=session();auto next=themeworkbench::makeDraft(ThemeFileData{});next.current.light.typography=draft().current.light.typography;next.current.dark.typography=draft().current.dark.typography;next.current.name=tr("theme_wb.untitled");a.drafts.push_back(std::move(next));a.active=a.drafts.size()-1;prepareDraft();app::requestUpdate();},false,s.compare);
    }).build();
}
} // namespace neo::theme_workbench_ui
