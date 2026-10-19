#pragma once
#include "ui/theme_workbench_state.h"
#include "model/settings.h"
#include "eui/platform.h"
#include <filesystem>

namespace neo {
inline void requestThemeWorkbenchClose(AppState& state);
inline void handleThemeWorkbenchEscape(AppState& state);
inline bool allowThemeWorkbenchWindowClose(AppState& state);
namespace theme_workbench_ui {
inline const char* tr(const char* id) { return i18n::tr(id); }
inline std::string bytes(const themeworkbench::Draft& d) {
    return themeloader::serializeWorkbench(d.current,d.baseline,d.sourcePath);
}
inline bool dirty(std::size_t index) {
    const auto& s=session();const auto it=s.cleanState.find(index);
    return it==s.cleanState.end() || it->second!=bytes(s.drafts[index]);
}
inline bool anyDirty() {
    for(std::size_t i=0;i<session().drafts.size();++i)if(dirty(i))return true;
    return false;
}
inline const char* variantKey(int i) {
    constexpr const char* ids[]{"neutral","soft","vivid"};return ids[std::clamp(i,0,2)];
}
inline const char* variantLabel(int i) {
    constexpr const char* ids[]{"theme_wb.neutral","theme_wb.soft","theme_wb.vivid"};return tr(ids[std::clamp(i,0,2)]);
}
inline ThemeAutoRecipe recipe(int i) {
    ThemeAutoRecipe r;r.seed=session().candidateSeed;r.variant=variantKey(i);r.round=session().autoRound;return r;
}
inline bool hasImportedColors() {
    const auto& report=draft().current.importReport;
    return std::any_of(report.begin(),report.end(),[](const auto& e){return e.imported;});
}
inline themeworkbench::Draft candidateDraft(int i) {
    themeworkbench::Draft copy;copy.current=draft().current;copy.baseline=draft().baseline;
    if(i>=0&&static_cast<std::size_t>(i)<session().candidates.size())
        themeworkbench::adoptGenerated(copy,session().candidates[i],recipe(i),session().missingOnly);
    return copy;
}
inline EditorColors shownColors() {
    auto& s=session();
    if(!s.compare&&s.trial>=0)return themeworkbench::colors(candidateDraft(s.trial),s.light);
    return themeworkbench::colors(draft(),s.light,s.compare);
}
inline eui::Color selectedColor() {
    const auto* f=findThemeColorField(session().selected);return f?shownColors().*(f->member):rgba(0,0,0);
}
inline bool readOnly(){return session().compare||session().trial>=0;}
inline void syncInputs() {
    auto& s=session();const auto c=selectedColor();s.hexDraft=hexColor(c,true);
    s.alphaDraft=std::to_string(static_cast<int>(std::lround(c.a*100)));
    const auto h=toHsv(c);s.pickH=h.h;s.pickS=h.s;s.pickV=h.v;s.pickA=c.a;
}
inline void select(const std::string& key) {
    if(!themeworkbench::editableField(key))return;
    auto& s=session();s.selected=key;s.layer=Layer::Color;s.panelHidden=false;
    s.seedEditing=false;s.panelScroll=0;s.hovered.clear();s.dragging=false;syncInputs();app::requestUpdate();
}
inline bool generateCandidates() {
    auto& s=session();eui::Color seed;
    if(!parseHex(s.seedHex,seed)||(s.seedHex.size()!=7&&s.seedHex.size()!=6)){
        s.message=tr("theme_wb.invalid_seed");s.trial=-1;s.candidates.clear();app::requestUpdate();return false;
    }
    const auto canonical=hexColor(seed,false);std::vector<ThemeFileData> next;
    for(int i=0;i<3;++i){ThemeAutoRecipe r;r.seed=canonical;r.variant=variantKey(i);r.round=s.autoRound;
        ThemeFileData data;std::string error;
        if(!themeauto::generate(r,data,error)){s.message=error.empty()?tr("theme_wb.generate_failed"):error;app::requestUpdate();return false;}
        next.push_back(std::move(data));
    }
    s.candidateSeed=canonical;s.candidates=std::move(next);s.trial=-1;s.message.clear();app::requestUpdate();return true;
}
inline void prepareDraft() {
    auto& s=session();s.layer=Layer::Palette;s.compare=false;s.trial=-1;s.draftMenu=false;
    s.seedEditing=false;s.dragging=false;s.panelHidden=false;s.shareType=false;
    s.panelScroll=s.typeScroll=s.previewScroll=s.previewSidebarScroll=0;s.hovered.clear();s.message.clear();s.missingOnly=true;
    const auto& r=draft().current.autoRecipe;s.seedHex=r?r->seed:"#62866B";s.autoRound=r?r->round:0;
    generateCandidates();syncInputs();
}
inline void ensureDraft(AppState& state) {
    auto& s=session();if(!s.drafts.empty())return;
    s.drafts.push_back(themeworkbench::makeDraft(activeTheme(),state.themeFile));s.active=0;
    s.cleanState[0]=bytes(draft());prepareDraft();
}
inline void leave(AppState& state) {
    auto& s=session();s.open=s.closeDialog=s.saveDialog=s.reviewingImport=s.dragging=false;
    s.closeAfterSave=s.windowClosePending=false;s.trial=-1;s.compare=false;
    s.catalogRefreshNeeded=true;state.settingsOpen=true;state.themeListOpen=true;app::requestUpdate();
}
inline void finishWindowClose() {
    auto& s=session();s.windowCloseApproved=true;s.windowClosePending=false;
    s.open=s.closeDialog=s.saveDialog=s.reviewingImport=false;
    app::requestClose();
}
inline bool reviewFile(AppState& state,const std::string& path) {
    ThemeFileData data;std::string error;
    if(!themeloader::read(path,data,error)){session().message=error.empty()?tr("theme_wb.read_failed"):error;showToast(state,tr("theme_wb.import_failed"),session().message);app::requestUpdate();return false;}
    ensureDraft(state);auto& s=session();s.pendingImport=std::move(data);s.pendingPath=path;
    s.reviewingImport=true;s.reportDetailsOpen=false;s.reportScroll=0;s.saveDialog=s.closeDialog=false;
    s.open=true;s.trial=-1;s.compare=false;s.message.clear();state.settingsOpen=false;app::requestUpdate();return true;
}
inline void chooseFile(AppState& state) {
    eui::platform::FileDialogOptions o;o.prompt=tr("theme_wb.choose_file");o.allowedExtensions={"json","css"};o.filterName=tr("theme_wb.theme_files");
    const auto r=eui::platform::openFileDialog(o);if(r.status==eui::platform::FileDialogStatus::Cancelled)return;
    if(!r.selected()){session().message=r.error.empty()?tr("theme_wb.read_failed"):r.error;showToast(state,tr("theme_wb.import_failed"),session().message);app::requestUpdate();return;}
    reviewFile(state,r.paths.front());
}
inline bool save(AppState& state,bool apply) {
    auto& s=session();auto& d=draft();std::string error;
    if(d.current.name.empty())d.current.name=tr("theme_wb.untitled");
    const auto directory=textfile::pathToUtf8(textfile::pathFromUtf8(settings::configDirectory())/"themes");
    const auto receipt=s.saveReceipts.find(s.active);std::error_code ec;
    bool reuse=false;
    if(receipt!=s.saveReceipts.end()&&receipt->second.first==bytes(d)&&std::filesystem::is_regular_file(textfile::pathFromUtf8(receipt->second.second),ec)){
        const auto saved=textfile::load(receipt->second.second,8u*1024u*1024u);
        reuse=saved.ok&&saved.document.text==receipt->second.first;
    }
    if(reuse)d.savedPath=receipt->second.second;
    else {
        if(!themeworkbench::saveDraft(d,directory,error)){s.message=error.empty()?tr("theme_wb.save_failed"):error;app::requestUpdate();return false;}
        s.saveReceipts[s.active]={bytes(d),d.savedPath};
    }
    s.cleanState[s.active]=bytes(d);s.catalogRefreshNeeded=true;
    if(apply){themeloader::activate(d.current,d.savedPath);state.themeFile=d.savedPath;mutableAppConfig().clearColorValue=editorColors().window;persistSettings(state);app::detail::requestFullPaint();}
    s.message=tr(apply?"theme_wb.applied":"theme_wb.saved");
    showToast(state,s.message,d.savedPath);s.saveDialog=false;
    if(s.closeAfterSave){
        if(s.windowClosePending){
            // Saving one draft must not silently discard the other drafts.
            if(anyDirty()){s.closeDialog=true;s.closeAfterSave=false;}
            else finishWindowClose();
        }else leave(state);
    }
    app::requestUpdate();return true;
}
inline void openSave(bool closeAfter=false) {
    auto& s=session();s.saveDialog=true;s.closeDialog=false;s.closeAfterSave=closeAfter;
    s.trial=-1;s.compare=false;s.message.clear();app::requestUpdate();
}
// A captured drag creates a checkpoint only at its first effective edit.
inline void changePicker(float fraction,int channel,bool seed) {
    auto& s=session();fraction=std::clamp(fraction,0.0f,1.0f);
    if(channel==0)s.pickH=fraction;if(channel==1)s.pickS=fraction;if(channel==2)s.pickV=fraction;if(channel==3)s.pickA=fraction;
    auto color=fromHsv(s.pickH,s.pickS,s.pickV);color.a=s.pickA;
    if(seed){s.seedHex=hexColor(color,false);generateCandidates();}
    else {
        if(readOnly())return;const auto p=themeworkbench::colors(draft(),s.light);const auto* f=findThemeColorField(s.selected);
        if(f&&(!themeworkbench::sameColor(p.*(f->member),color)||!themeworkbench::isLocked(draft(),s.light,s.selected))){themeworkbench::setColor(draft(),s.light,s.selected,color,!s.dragRecorded);s.dragRecorded=true;}
        s.hexDraft=hexColor(color,true);s.alphaDraft=std::to_string(static_cast<int>(std::lround(color.a*100)));
    }
    app::requestUpdate();
}
} // namespace theme_workbench_ui

inline bool themeWorkbenchOpen(){return theme_workbench_ui::session().open;}
inline void requestThemeWorkbenchClose(AppState& state) {
    using namespace theme_workbench_ui;
    if(anyDirty()){auto& s=session();s.windowClosePending=false;s.closeDialog=true;s.saveDialog=s.reviewingImport=false;s.trial=-1;s.dragging=false;app::requestUpdate();}
    else leave(state);
}
inline void handleThemeWorkbenchEscape(AppState& state) {
    using namespace theme_workbench_ui;auto& s=session();
    if(s.reviewingImport){s.reviewingImport=false;s.pendingPath.clear();}
    else if(s.saveDialog){s.saveDialog=false;s.closeAfterSave=false;s.windowClosePending=false;}
    else if(s.closeDialog){s.closeDialog=false;s.windowClosePending=false;}
    else if(s.seedEditing)s.seedEditing=false;
    else if(s.compare){s.compare=false;syncInputs();}
    else if(s.layer!=Layer::Palette){s.layer=Layer::Palette;s.panelScroll=0;s.seedEditing=false;}
    else requestThemeWorkbenchClose(state);
    app::requestUpdate();
}
inline bool allowThemeWorkbenchWindowClose(AppState& state) {
    using namespace theme_workbench_ui;auto& s=session();
    if(s.windowCloseApproved){s.windowCloseApproved=false;s.open=false;return true;}
    if(!anyDirty()){s.open=false;return true;}
    // This also protects retained drafts after the user has left the workbench.
    s.windowClosePending=true;s.open=true;s.closeDialog=true;s.saveDialog=s.reviewingImport=false;
    s.trial=-1;s.dragging=false;state.settingsOpen=false;app::requestUpdate();return false;
}
inline void openThemeWorkbench(AppState& state) {
    using namespace theme_workbench_ui;ensureDraft(state);auto& s=session();
    s.open=true;s.reviewingImport=s.saveDialog=s.closeDialog=false;state.settingsOpen=false;prepareDraft();app::requestUpdate();
}
inline void themeWorkbenchImportFile(AppState& state){theme_workbench_ui::ensureDraft(state);theme_workbench_ui::chooseFile(state);}
} // namespace neo
