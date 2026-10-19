#pragma once

// 单一工作区：左侧样张，右侧单层配色 / 单色 / 排版。
// 导入先审阅，候选悬停只试览，保存只新建文件；返回主题库保留会话草稿。
#include "ui/theme_workbench_dialogs.h"
#include "ui/theme_workbench_preview.h"

namespace neo {
inline void themeWorkbenchOverlay(eui::Ui& ui,AppState& state,const eui::Screen& screen) {
    using namespace theme_workbench_ui;
    auto& s=session();if(!s.open||s.drafts.empty())return;
    const EditorColors c=editorColors(state.theme);const Dim d=dims(state);
    const float width=screen.width,chromeHeight=uiMetrics(state).menuBarHeight;
    const float height=std::max(160.0f,screen.height-chromeHeight);
    const bool compact=width<760;
    const float header=d.control+d.pad,tools=d.control+d.gap*2,footer=d.control+d.pad;
    const float bodyY=header+tools,bodyH=std::max(60.0f,height-bodyY-footer);
    const float panelW=s.panelHidden?0:std::min(std::max(270.0f,d.font*22),width*(compact?.50f:.35f));
    const float previewW=width-panelW;
    ui.stack("themeWorkbench.root").position(0,chromeHeight).size(width,height).zIndex(1100).content([&]{
        ui.rect("themeWorkbench.background").fill().color(c.window).build();
        ui.rect("themeWorkbench.header").size(width,header).color(c.toolbar).build();
        label(ui,"themeWorkbench.title",tr("theme_wb.title"),d.pad,0,std::max(60.0f,width-d.pad*2-140),header,c.text,d.title,600);
        ghostButton(ui,"themeWorkbench.back",tr("theme_wb.library"),width-d.pad-132,(header-d.control)*.5f,132,d.control,c,d.font,[&state]{requestThemeWorkbenchClose(state);},false,false,UiIcon::ArrowLeft);
        hairline(ui,"themeWorkbench.header.line",0,header-1,width,1,c);
        const float segmentW=std::min(170.0f,std::max(116.0f,previewW*.3f));
        segmented(ui,"themeWorkbench.mode",d.pad,header+d.gap,segmentW,d.control,c,d.hint,
            {tr("theme_wb.light"),tr("theme_wb.dark")},s.light?0:1,[](int index){auto& a=session();a.light=index==0;a.seedEditing=false;a.trial=-1;syncInputs();app::requestUpdate();});
        segmented(ui,"themeWorkbench.scene",d.pad+segmentW+d.gap,header+d.gap,segmentW,d.control,c,d.hint,
            {tr("theme_wb.reading"),tr("theme_wb.code")},s.codeView?1:0,[](int index){session().codeView=index==1;session().previewScroll=0;app::requestUpdate();});
        if(!compact)label(ui,"themeWorkbench.preview.hint",tr(s.compare?"theme_wb.comparing":s.trial>=0?"theme_wb.trying":"theme_wb.preview_help"),d.pad+segmentW*2+d.gap*2,header+d.gap,std::max(1.0f,width-d.pad*2-segmentW*2-d.gap*2-130),d.control,c.textMuted,d.hint);
        ghostButton(ui,"themeWorkbench.panel.toggle",tr(s.panelHidden?"theme_wb.show_settings":"theme_wb.hide_settings"),width-d.pad-120,header+d.gap,120,d.control,c,d.hint,[]{session().panelHidden=!session().panelHidden;app::requestUpdate();},!s.panelHidden);
        auto shown=shownColors();const auto& theme=s.compare?draft().baseline:draft().current;
        const auto typography=(s.light?theme.light:theme.dark).typography;
        theme_workbench_preview::Args a;a.colors=&shown;a.typography=&typography;
        a.bodyPx=std::max(10.0f,state.editorFontSize);a.bodyFont=editorFontFamily(state);a.codeFont=codeFontFamily(state);
        a.codeView=s.codeView;a.interactive=!s.compare&&s.trial<0;a.selected=s.layer==Layer::Color?s.selected:"";
        a.hovered=s.hovered;a.marker=c.accent;a.scroll=&s.previewScroll;a.sidebarScroll=&s.previewSidebarScroll;
        a.onSelect=[](const std::string& key){select(key);};
        a.onHover=[](const std::string& key,bool inside){auto& value=session().hovered;if(inside)value=key;else if(value==key)value.clear();app::requestUpdate();};
        theme_workbench_preview::draw(ui,"themeWorkbench.preview",d.pad,bodyY,std::max(100.0f,previewW-d.pad*2),bodyH-d.gap,a);
        if(panelW>0){
            ui.rect("themeWorkbench.panel.surface").position(previewW,bodyY).size(panelW,bodyH).color(c.panel).build();
            hairline(ui,"themeWorkbench.panel.border",previewW,bodyY,1,bodyH,c);
            const bool type=s.layer==Layer::Type;
            components::scrollView(ui,type?"themeWorkbench.type.scroll":"themeWorkbench.panel.scroll")
                .position(previewW+1,bodyY).size(std::max(80.0f,panelW-1),bodyH).theme(c.tokens)
                .offset(type?s.typeScroll:s.panelScroll).step(d.row*2)
                .onChange([type](float value){if(type)session().typeScroll=value;else session().panelScroll=value;})
                .content([&](eui::Ui& content,float availableWidth,float){
                    if(s.layer==Layer::Color)colorLayer(content,availableWidth,c,d);
                    else if(s.layer==Layer::Type)typeLayer(content,availableWidth,c,d,std::max(10.0f,state.editorFontSize));
                    else paletteLayer(content,state,availableWidth,c,d);
                }).build();
        }
        const float fy=height-footer;
        ui.rect("themeWorkbench.footer").position(0,fy).size(width,footer).color(c.toolbar).build();
        hairline(ui,"themeWorkbench.footer.line",0,fy,width,1,c);
        const float by=fy+(footer-d.control)*.5f;
        ghostButton(ui,"themeWorkbench.undo",tr("theme_wb.undo"),d.pad,by,58,d.control,c,d.hint,[]{themeworkbench::undo(draft());session().trial=-1;session().seedEditing=false;syncInputs();app::requestUpdate();},false,s.compare||draft().undoStack.empty());
        ghostButton(ui,"themeWorkbench.redo",tr("theme_wb.redo"),d.pad+62,by,58,d.control,c,d.hint,[]{themeworkbench::redo(draft());session().trial=-1;session().seedEditing=false;syncInputs();app::requestUpdate();},false,s.compare||draft().redoStack.empty());
        ghostButton(ui,"themeWorkbench.compare",tr(s.compare?"theme_wb.current":"theme_wb.compare"),d.pad+128,by,112,d.control,c,d.hint,[]{session().compare=!session().compare;session().trial=-1;session().seedEditing=false;syncInputs();app::requestUpdate();},s.compare);
        const float saveW=compact?94.0f:124.0f;
        if(width>800){
            std::string message=s.message;if(message.empty())message=draft().current.name.empty()?tr("theme_wb.untitled"):draft().current.name;
            if(dirty(s.active))message+=" *";
            label(ui,"themeWorkbench.message",message,d.pad+252,by,std::max(1.0f,width-d.pad*2-252-saveW-d.gap),d.control,c.textMuted,d.hint);
        }
        primaryButton(ui,"themeWorkbench.save.open",tr("theme_wb.save"),width-d.pad-saveW,by,saveW,d.control,c,d.font,[]{openSave();});
        if(s.reviewingImport)importDialog(ui,state,width,height,c,d);
        else if(s.saveDialog)saveDialogView(ui,state,width,height,c,d);
        else if(s.closeDialog)closeDialogView(ui,state,width,height,c,d);
    }).build();
}
} // namespace neo
