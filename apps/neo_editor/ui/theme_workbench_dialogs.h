#pragma once
#include "ui/theme_workbench_panels.h"
#include "components/virtuallist.h"

namespace neo::theme_workbench_ui {

inline void dialog(eui::Ui& ui,AppState& state,float width,float height,float preferredH,
                   const EditorColors& c,const Dim& d,const char* title,
                   const std::function<void(float,float)>& body) {
    const float w=std::max(240.0f,std::min(width-24,660.0f));
    const float h=std::max(180.0f,std::min(height-24,preferredH));
    ui.stack("themeWorkbench.dialog").size(width,height).zIndex(2300).content([&]{
        ui.rect("themeWorkbench.dialog.shade").fill().color(rgba(0,0,0,.34f))
            .preserveFocusOnPress().onClick([]{}).build();
        ui.stack("themeWorkbench.dialog.card").position((width-w)*.5f,(height-h)*.5f).size(w,h).clip().content([&]{
            ui.rect("themeWorkbench.dialog.surface").fill().radius(neo::corner(c, 10.0f)).color(c.panel).border(1,c.border)
                .preserveFocusOnPress().onClick([]{}).build();
            label(ui,"themeWorkbench.dialog.title",tr(title),d.pad,10,w-d.pad*2-38,d.row,c.text,d.title,600);
            ghostButton(ui,"themeWorkbench.dialog.close","",w-d.pad-32,12,32,32,c,d.font,
                [&state]{handleThemeWorkbenchEscape(state);},false,false,UiIcon::Close);
            body(w,h);
        }).build();
    }).build();
}

inline void importDialog(eui::Ui& ui,AppState& state,float width,float height,const EditorColors& c,const Dim& d) {
    auto& s=session();
    dialog(ui,state,width,height,570,c,d,"theme_wb.import_review",[&](float w,float h){
        const float inner=w-d.pad*2,top=d.row+18,footer=h-d.control-d.pad;
        components::scrollView(ui,"themeWorkbench.report.scroll").position(d.pad,top)
            .size(inner,std::max(40.0f,footer-top-d.gap)).theme(c.tokens).offset(s.reportScroll)
            .onChange([](float value){session().reportScroll=value;})
            .content([&](eui::Ui& content,float cw,float){
                const auto rows=s.pendingImport.importReport;
                std::size_t light=0,dark=0;
                for(const auto& item:rows)if(item.imported)(item.light?light:dark)++;
                const float contentH=d.row*6+(s.reportDetailsOpen?rows.size()*d.row*2:0);
                content.stack("themeWorkbench.report.content").size(cw,contentH).content([&]{
                    float y=0;
                    label(content,"themeWorkbench.report.name",s.pendingImport.name,d.gap,y,cw-d.gap*2,d.row,c.text,d.font,600);y+=d.row;
                    label(content,"themeWorkbench.report.count",std::string(tr("theme_wb.light"))+" "+std::to_string(light)+"/26  ·  "+tr("theme_wb.dark")+" "+std::to_string(dark)+"/26",d.gap,y,cw-d.gap*2,d.row,c.accent,d.font);y+=d.row;
                    paragraph(content,"themeWorkbench.report.note",tr("theme_wb.import_note"),d.gap,y,cw-d.gap*2,d.row*2.5f,c.textMuted,d.hint);y+=d.row*2.5f;
                    ghostButton(content,"themeWorkbench.report.details",tr(s.reportDetailsOpen?"theme_wb.hide_report":"theme_wb.show_report"),0,y,cw,d.row,c,d.font,[]{session().reportDetailsOpen=!session().reportDetailsOpen;app::requestUpdate();},s.reportDetailsOpen);y+=d.row;
                    if(s.reportDetailsOpen)for(std::size_t i=0;i<rows.size();++i){
                        const auto& item=rows[i];const auto id="themeWorkbench.report.item."+std::to_string(i);
                        label(content,id+".label",std::string(tr(item.light?"theme_wb.light":"theme_wb.dark"))+" · "+fieldLabel(item.field),d.gap,y,cw-d.gap*2,d.row,c.text,d.font);y+=d.row;
                        paragraph(content,id+".reason",std::string(tr(item.imported?"theme_wb.import_ok":"theme_wb.import_default"))+" · "+(item.imported?item.source:item.reason),d.gap,y,cw-d.gap*2,d.row,c.textMuted,d.hint);y+=d.row;
                    }
                }).build();
            }).build();
        const float half=(inner-d.gap)*.5f;
        ghostButton(ui,"themeWorkbench.report.cancel",tr("theme_wb.cancel"),d.pad,footer,half,d.control,c,d.font,[]{session().reviewingImport=false;session().pendingPath.clear();app::requestUpdate();});
        primaryButton(ui,"themeWorkbench.report.confirm",tr("theme_wb.create_import"),d.pad+half+d.gap,footer,half,d.control,c,d.font,[]{
            auto& a=session();a.drafts.push_back(themeworkbench::makeDraft(a.pendingImport,a.pendingPath));
            a.active=a.drafts.size()-1;a.reviewingImport=false;a.pendingPath.clear();prepareDraft();app::requestUpdate();
        });
    });
}

inline void saveDialogView(eui::Ui& ui,AppState& state,float width,float height,const EditorColors& c,const Dim& d) {
    auto& s=session();
    dialog(ui,state,width,height,450,c,d,"theme_wb.save_title",[&](float w,float h){
        const float inner=w-d.pad*2,top=d.row+18,footer=h-d.control-d.pad;
        components::scrollView(ui,"themeWorkbench.save.scroll").position(d.pad,top).size(inner,std::max(40.0f,footer-top-d.gap))
            .theme(c.tokens).content([&](eui::Ui& content,float cw,float){
                content.stack("themeWorkbench.save.content").size(cw,d.row*9+d.control+d.gap).content([&]{
                    float y=0;
                    label(content,"themeWorkbench.save.name.label",tr("theme_wb.name"),0,y,cw,d.row,c.text,d.font);y+=d.row;
                    components::input(content,"themeWorkbench.save.name").position(0,y).size(cw,d.control).value(draft().current.name)
                        .placeholder(tr("theme_wb.untitled")).fontSize(d.font).theme(c.tokens)
                        .onChange([](const std::string& value){draft().current.name=value;app::requestUpdate();}).build();y+=d.control+d.gap;
                    paragraph(content,"themeWorkbench.save.note",tr("theme_wb.save_note"),0,y,cw,d.row*2,c.textMuted,d.hint);y+=d.row*2;
                    const std::string summary=std::string(tr("theme_wb.light"))+" "+std::to_string(themeworkbench::changedCount(draft(),true))+" / 26  ·  "+tr("theme_wb.dark")+" "+std::to_string(themeworkbench::changedCount(draft(),false))+" / 26";
                    label(content,"themeWorkbench.save.summary",summary,0,y,cw,d.row,c.text,d.font);y+=d.row;
                    paragraph(content,"themeWorkbench.save.count_note",tr("theme_wb.count_note"),0,y,cw,d.row*2,c.textMuted,d.hint);y+=d.row*2;
                    if(!s.message.empty())paragraph(content,"themeWorkbench.save.error",s.message,0,y,cw,d.row*2,c.text,d.hint);
                }).build();
            }).build();
        const float half=(inner-d.gap)*.5f;
        ghostButton(ui,"themeWorkbench.save.only",tr("theme_wb.save_only"),d.pad,footer,half,d.control,c,d.font,[&state]{save(state,false);});
        primaryButton(ui,"themeWorkbench.save.apply",tr("theme_wb.save_apply"),d.pad+half+d.gap,footer,half,d.control,c,d.font,[&state]{save(state,true);});
    });
}

inline void closeDialogView(eui::Ui& ui,AppState& state,float width,float height,const EditorColors& c,const Dim& d) {
    auto& s=session();
    dialog(ui,state,width,height,490,c,d,"theme_wb.unsaved_title",[&](float w,float h){
        const float inner=w-d.pad*2,top=d.row+18,footer=h-d.control*2-d.gap-d.pad;
        components::scrollView(ui,"themeWorkbench.close.scroll").position(d.pad,top).size(inner,std::max(40.0f,footer-top-d.gap))
            .theme(c.tokens).content([&](eui::Ui& content,float cw,float){
                std::size_t count=0;for(std::size_t i=0;i<s.drafts.size();++i)if(dirty(i))++count;
                content.stack("themeWorkbench.close.content").size(cw,d.row*(count+3)).content([&]{
                    paragraph(content,"themeWorkbench.close.note",tr(s.windowClosePending?"theme_wb.unsaved_exit":"theme_wb.unsaved_library"),0,0,cw,d.row*3,c.textMuted,d.hint);
                    float y=d.row*3;
                    for(std::size_t i=0;i<s.drafts.size();++i)if(dirty(i)){
                        const auto name=s.drafts[i].current.name.empty()?std::string(tr("theme_wb.untitled")):s.drafts[i].current.name;
                        ghostButton(content,"themeWorkbench.close.draft."+std::to_string(i),name+" *",0,y,cw,d.row,c,d.font,[i]{session().active=i;syncInputs();app::requestUpdate();},s.active==i);y+=d.row;
                    }
                }).build();
            }).build();
        const float half=(inner-d.gap)*.5f;
        ghostButton(ui,"themeWorkbench.close.cancel",tr("theme_wb.continue_edit"),d.pad,footer,half,d.control,c,d.font,[]{session().closeDialog=false;session().windowClosePending=false;app::requestUpdate();});
        primaryButton(ui,"themeWorkbench.close.save",tr("theme_wb.save_draft"),d.pad+half+d.gap,footer,half,d.control,c,d.font,[]{
            if(!dirty(session().active))for(std::size_t i=0;i<session().drafts.size();++i)if(dirty(i)){session().active=i;break;}
            openSave(true);
        });
        ghostButton(ui,"themeWorkbench.close.leave",tr(s.windowClosePending?"theme_wb.exit_unsaved":"theme_wb.keep_library"),d.pad,footer+d.control+d.gap,inner,d.control,c,d.font,[&state]{if(session().windowClosePending)finishWindowClose();else leave(state);});
    });
}

} // namespace neo::theme_workbench_ui
