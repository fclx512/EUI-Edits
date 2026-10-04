#pragma once

#include "model/font_catalog.h"
#include "model/theme_loader.h"
#include "model/settings.h"
#include "platform/file_assoc.h"
#include "platform/font_safety.h"
#include "eui/json.h"
#include "state/app_actions.h"
#include "ui/metrics.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/ui_language.h"
#include "ui/about_page.h"
#include "model/version.h"
#include "core/platform/bundled_resources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

// 主窗口内的设置页。分类、字体选择和主题库共用一棵 UI 树；颜色统一来自
// editorColors()，字号来自 uiMetrics()。编辑器树在设置打开时不参与 compose，
// 输入组件的状态仍保留在 Ui::StateStore 中，返回后继续原文稿和滚动位置。
namespace neo {
namespace settings_detail {

inline constexpr float kPadding = 26.0f;
// 行高按界面字号计算，避免放大字号后标签与说明互压。
inline constexpr float kControlHeight = 34.0f;
inline constexpr float kStepButtonWidth = 30.0f;
inline constexpr float kButtonWidth = 104.0f;
inline constexpr float kButtonHeight = 30.0f;
// 控件列宽度。原来是 132：两个字体按钮各分到 63px，"编辑器 微软雅黑"必然被截成
// "编辑器…"，这就是字体按钮看着难受的根因。176 能放下 8 个汉字 + 一个箭头。
inline constexpr float kControlWidth = 176.0f;

// 主内容区与字体/主题子页共用的几何数据。
struct PanelGeometry {
    float contentWidth = 0.0f;
    float controlWidth = kControlWidth;
    float controlX = 0.0f;
    float labelWidth = 0.0f;
    float rowHeight = 84.0f;
    float footerRuleY = 0.0f;
    float buttonY = 0.0f;
    bool stacked = false;
    float controlTop = 34.0f;
    float cardHeight = 100.0f;
};

// 绝对定位的按钮。toolButton 不带坐标，而设置面板是逐行手排的，所以这里单开一个收口。
inline void flatButton(eui::Ui& ui,
                       const EditorColors& colors,
                       const std::string& id,
                       const std::string& label,
                       float x,
                       float y,
                       float width,
                       float height,
                       bool accent,
                       float fontSize,
                       std::function<void()> onClick, bool danger = false) {
    danger = danger || id == "settings.fontPicker.reset";
    eui::Color normal = transparentColor();
    eui::Color textColor = colors.text;
    eui::Color hover = colors.rowHover;
    eui::Color pressed = colors.pressed;
    if (accent) {
        normal = rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.16f);
        textColor = colors.accent;
    }
    if(danger) {
        textColor = colors.text.r > .5f ? rgba(1.0f,.43f,.46f,1) : rgba(.78f,.12f,.16f,1);
        normal=rgba(textColor.r,textColor.g,textColor.b,.14f);
        hover=rgba(textColor.r,textColor.g,textColor.b,.24f);
        pressed=rgba(textColor.r,textColor.g,textColor.b,.32f);
    }
    components::button(ui, id)
        .position(x, y)
        .size(width, height)
        .radius(6.0f)
        .fontSize(fontSize)
        .text(label)
        .textColor(textColor)
        .colors(normal, hover, pressed)
        .shadow(0.0f, 0.0f, 0.0f, transparentColor())
        .border(0.0f, transparentColor())
        .preserveFocusOnPress()
        .onClick(std::move(onClick))
        .transition(quickTransition())
        .build();
}

// ── 主题文件入口（T13 第一阶段）──────────────────────────────────────────
// 实现放在使用它的这一层（设置面板的行回调），而不是 state/app_actions.cpp：
// 本轮只允许改那个 .cpp 里**已有**的函数，新入口跟着行留在本文件。
// 三步顺序固定：校验并生效 → 同步状态与清屏色 → 落盘 + 整屏失效。
// 失败（文件不存在 / JSON 坏 / 版本不认识）只弹 toast 说明，**不动当前配色**。

inline bool applyThemeFilePath(AppState& state, const std::string& path) {
    std::string error;
    if (!themeloader::load(path, error)) {
        showToast(state, i18n::tr("settings.theme_unavailable"), error);
        app::requestUpdate();
        return false;
    }
    state.themeFile = path;
    // 主题只覆盖 base 那一侧的配色，所以外观要跟着切过去，否则用户看不到自己选的主题。
    // （配色数据本体在 style_schema.h 的 activeTheme()，themeloader::load 刚写完它。）
    if (activeTheme().version == themeloader::kSchemaVersion) {
        state.theme = activeTheme().baseLight ? ThemeMode::Light : ThemeMode::Dark;
    }
    // 清屏色与 applyTheme 同一条同步（启动后只在配置读一次）。
    mutableAppConfig().clearColorValue = editorColors().window;
    persistSettings(state);
    if (activeTheme().version >= themeloader::kSchemaVersion2) {
        const ThemeFileData& theme = activeTheme();
        const std::string summary = i18n::format("settings.theme_counts",{
            {"name",theme.name},{"light",std::to_string(theme.light.colors.size())},
            {"dark",std::to_string(theme.dark.colors.size())},
            {"unmapped",std::to_string(56-theme.light.colors.size()-theme.dark.colors.size())}});
        showToast(state, i18n::tr("settings.theme_loaded"), summary);
    }
    // 颜色烘在图元里：保留层的旧色必须整屏作废（与 applyTheme 同一套失效通知）。
    app::requestUpdate();
    app::detail::requestFullPaint();
    return true;
}

inline void resetThemeFile(AppState& state) {
    if (state.themeFile.empty()) {
        return;
    }
    themeloader::reset();
    state.themeFile.clear();
    mutableAppConfig().clearColorValue = editorColors().window;
    persistSettings(state);
    app::requestUpdate();
    app::detail::requestFullPaint();
}

// 选主题文件。文件对话框用的是**打开文档**同一条框架约定
// （eui::platform::openFileDialog，native_dialogs 只有"选目录/另存为"两个补位）。
inline bool chooseThemeFile(AppState& state) {
    eui::platform::FileDialogOptions options;
    options.prompt = i18n::tr("settings.pick_theme");
    options.allowedExtensions = {"json", "css"};
    options.filterName = i18n::tr("settings.theme_file");
    // 已加载主题时从同目录开始；首次选择交给系统文件对话框使用最近的位置。
    // 主题库目录可能不存在，把它硬塞给 lpstrInitialDir 会让首次选择难以定位下载文件。
    if (!state.themeFile.empty()) {
        options.initialDirectory = textfile::parentPath(state.themeFile);
    }

    const eui::platform::FileDialogResult result = eui::platform::openFileDialog(options);
    if (result.status == eui::platform::FileDialogStatus::Cancelled) {
        return false;
    }
    if (!result.selected()) {
        showToast(state, i18n::tr("settings.pick_theme_failed"),
                  result.error.empty() ? i18n::tr("settings.dialog_no_file") : result.error);
        return false;
    }
    return applyThemeFilePath(state, result.paths.front());
}

struct ThemeCatalogEntry {
    std::string name;
    std::string path;
};

inline std::vector<ThemeCatalogEntry>& themeCatalog() {
    static std::vector<ThemeCatalogEntry> entries;
    return entries;
}

inline void refreshThemeCatalog() {
    namespace fs = std::filesystem;
    std::vector<ThemeCatalogEntry>& entries = themeCatalog();
    entries.clear();
    const fs::path directory = textfile::pathFromUtf8(settings::configDirectory()) / "themes";
    std::error_code error;
    if (!fs::is_directory(directory, error)) return;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        const fs::path path = it->path();
        std::error_code entryError;
        if (it->is_directory(entryError)) {
            const fs::path manifest = path / "manifest.json";
            if (!fs::is_regular_file(manifest, entryError) ||
                !fs::is_regular_file(path / "theme.css", entryError)) continue;
            if (fs::file_size(manifest, entryError) > 8ULL * 1024ULL * 1024ULL || entryError)
                continue;
            std::ifstream source(manifest, std::ios::binary);
            const std::string json{std::istreambuf_iterator<char>(source),
                                   std::istreambuf_iterator<char>()};
            eui::json::Document document;
            std::string name;
            if (!document.parse(json) ||
                !document.root().get("name").string(name) ||
                !document.root().get("minAppVersion").valid()) continue;
            entries.push_back({name, textfile::pathToUtf8(manifest)});
        } else if (it->is_regular_file(entryError) && path.extension() == ".json") {
            if (fs::file_size(path, entryError) > 8ULL * 1024ULL * 1024ULL || entryError)
                continue;
            std::ifstream source(path, std::ios::binary);
            const std::string json{std::istreambuf_iterator<char>(source),
                                   std::istreambuf_iterator<char>()};
            ThemeFileData data;
            std::string parseError;
            if (themeloader::parse(json, data, parseError)) {
                entries.push_back({data.name.empty() ? textfile::pathToUtf8(path.stem()) : data.name,
                                   textfile::pathToUtf8(path)});
            }
        } else if (it->is_regular_file(entryError) && path.extension() == ".css") {
            entries.push_back({textfile::pathToUtf8(path.stem()), textfile::pathToUtf8(path)});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
}

inline void openThemeChooser(AppState& state) {
    // 直接进主题库子页：浏览已收藏的主题是主路径，文件导入在库页里显式选择。
    // （原先先弹系统文件对话框、取消才落到库页——取消不该是进库的入口。）
    refreshThemeCatalog();
    state.themeListOpen = true;
    app::requestUpdate();
}

// 字体槽按钮：占满控制列，左边写"目标 · 字体名"，右边一个 → 箭头。
// 与 flatButton 的区别是它表达"点开去选"而不是"立刻执行"，所以文字左对齐 + 带箭头；
// 未自选时底色弱化、文字用正文色，自选后整块转成主色。
inline void slotButton(eui::Ui& ui,
                       const EditorColors& colors,
                       const char* fontFamily,
                       const UiMetrics& metrics,
                       const std::string& id,
                       const std::string& label,
                       float x,
                       float y,
                       float width,
                       bool highlighted,
                       std::function<void()> onClick) {
    const float height = kControlHeight;
    const float fontSize = metrics.panelFontSize;
    const float arrowWidth = 20.0f;
    const eui::Color normal = highlighted
        ? rgba(colors.accent.r, colors.accent.g, colors.accent.b, 0.16f)
        : rgba(colors.text.r, colors.text.g, colors.text.b, 0.07f);
    const eui::Color hover = colors.rowHover;

    ui.rect(id + ".bg")
        .x(x)
        .y(y)
        .size(width, height)
        .radius(6.0f)
        .states(normal, hover, colors.pressed)
        .cursor(eui::CursorShape::Hand)
        // 不让点设置面板把焦点从编辑区抢走（与其它按钮一致）。
        .preserveFocusOnPress()
        .onClick(std::move(onClick))
        .transition(quickTransition())
        .build();

    // 注意元素 id：同一行里 rowLabel 已经占了 `id + ".label"`，这里必须换后缀。
    // 两处同 id 时框架会把两个文本元素当成同一个，画出来的位置会错乱
    // （实测：标签位置画上了字体名，按钮上方还多出一份）。
    ui.text(id + ".value")
        .x(x + 10.0f)
        .y(y)
        .size(std::max(0.0f, width - arrowWidth - 14.0f), height)
        .text(label)
        .fontFamily(fontFamily)
        .fontSize(fontSize)
        .color(highlighted ? colors.accent : colors.text)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();

    iconView(ui, id + ".arrow", UiIcon::ArrowRight, x + width - arrowWidth - 2.0f,
             y + (height-16)*.5f, 16, highlighted ? colors.accent : colors.textMuted);
}

// 一行标签 + 可选说明。
inline void rowLabel(eui::Ui& ui,
                     const EditorColors& colors,
                     const UiMetrics& metrics,
                     const char* fontFamily,
                     const std::string& id,
                     const std::string& text,
                     const std::string& hint,
                     float x,
                     float y,
                     float width,
                     const PanelGeometry& geometry) {
    ui.rect(id + ".card")
        .position(x, y).size(geometry.contentWidth, geometry.cardHeight)
        .radius(8.0f).color(colors.editor)
        .border(1.0f, components::theme::withAlpha(colors.border, .45f)).build();
    // 文字块在卡片内竖向居中。说明行数按实际测量估算（单行/两行差一个行高），
    // 否则单行说明的卡片下方会空出一大块；标题↔说明之间保留 4px 间隙。
    const float labelLineH = metrics.panelLabelFontSize + 7.0f;
    const float hintLineH = metrics.panelHintFontSize + 5.0f;
    int hintLines = 1;
    if (!hint.empty()) {
        const float hintWidth = core::TextPrimitive::measureTextWidth(hint, fontFamily, metrics.panelHintFontSize);
        hintLines = std::max(1, static_cast<int>(std::ceil(hintWidth / std::max(1.0f, width))));
    }
    const float textAreaH = geometry.cardHeight - (geometry.stacked ? 14.0f + kControlHeight : 0.0f);
    const float contentH = labelLineH + 4.0f + static_cast<float>(hintLines) * hintLineH;
    const float textTop = y + std::max(6.0f, (textAreaH - contentH) * 0.5f);
    ui.text(id + ".label")
        .x(x + 18.0f)
        .y(textTop)
        .size(width, labelLineH)
        .text(text)
        .fontFamily(fontFamily)
        .fontSize(metrics.panelLabelFontSize).fontWeight(600)
        .color(colors.text)
        .build();
    if (!hint.empty()) {
        ui.text(id + ".hint")
            .x(x + 18.0f)
            .y(textTop + labelLineH + 4.0f)
            .size(width, static_cast<float>(hintLines) * hintLineH)
            .text(hint).wrap(true)
            .fontFamily(fontFamily)
            .fontSize(metrics.panelHintFontSize)
            .color(colors.textMuted)
            .verticalAlign(eui::VerticalAlign::Top)
            .build();
    }
}

// [−] 数值 [+]
inline void stepControl(eui::Ui& ui,
                        const EditorColors& colors,
                        const std::string& id,
                        float x,
                        float y,
                        float width,
                        const std::string& value,
                        bool canDecrease,
                        bool canIncrease,
                        std::function<void()> onDecrease,
                        std::function<void()> onIncrease,
                        const char* fontFamily,
                        float fontSize) {
    iconButton(ui, id+".minus", UiIcon::Minus, x, y, kControlHeight,
               canDecrease, std::move(onDecrease));
    iconButton(ui, id+".plus", UiIcon::Plus, x+width-kControlHeight, y, kControlHeight,
               canIncrease, std::move(onIncrease));
    ui.text(id + ".value")
        .x(x + kStepButtonWidth)
        .y(y)
        .size(std::max(0.0f, width - kStepButtonWidth * 2.0f), kControlHeight)
        .text(value)
        .fontFamily(fontFamily)
        .fontSize(fontSize)
        .color(colors.text)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
}

// segmented 不带坐标，套一层定位用的 stack。
inline void segmentedAt(eui::Ui& ui,
                        const EditorColors& colors,
                        const std::string& id,
                        float x,
                        float y,
                        float width,
                        const std::vector<std::string>& items,
                        int selected,
                        float fontSize,
                        std::function<void(int)> onChange) {
    ui.stack(id + ".slot")
        .x(x)
        .y(y)
        .size(width, kControlHeight)
        .content([&] {
            components::segmented(ui, id)
                .size(width, kControlHeight)
                .items(items)
                .selected(selected)
                .fontSize(fontSize)
                .theme(colors.tokens)
                .transition(quickTransition())
                .onChange(std::move(onChange))
                .build();
        })
        .build();
}

// 界面缩放按百分比展示：100% 表示跟随系统（框架已乘系统 DPI），往上是在其上额外放大。
inline std::string scaleText(const AppState& state) {
    return std::to_string(static_cast<int>(std::lround(state.uiScale * 100.0f))) + "%";
}

inline std::string systemScaleHint(const AppState& state) {
    return i18n::format(std::fabs(state.uiScale-1.0f)<.001f?"settings.system_scale_auto":"settings.system_scale_custom",
        {{"scale",std::to_string(static_cast<int>(std::lround(state.systemScale*100.0f)))}});
}

inline std::vector<const filetypes::Type*> associationRows(const AppState& state) {
    std::vector<const filetypes::Type*> rows;const auto needle=filetypes::lower(state.associationSearch);
    for(const auto& t:filetypes::associationTypes()) {
        const int category=t.category==filetypes::Category::Code?3:t.category==filetypes::Category::Data?2:1;
        if(state.associationCategory && category!=state.associationCategory) continue;
        if(!needle.empty() && (std::string(".")+t.extension+" "+filetypes::lower(filetypes::label(t.language))).find(needle)==std::string::npos) continue;
        rows.push_back(&t);
    }
    return rows;
}
inline float associationRowHeight(float width) { return width<500.0f?62.0f:54.0f; }
// availableHeight = 扣除卡片上方内容后的剩余视口高度。按窗口余量换算，
// 小窗口保底 2 行（保证列表可用），大窗口最多 9 行；页面滚动范围由 contentHeight 推出，会自动跟上。
inline float associationViewportHeight(float width,float availableHeight,const AppState& state) {
    const float rowH=associationRowHeight(width-36);
    const float rows=static_cast<float>(associationRows(state).size());
    const float fixed=(width-36<620?306.0f:264.0f)+18.0f; // 卡片头部(搜索/按钮) + 列表下沿留白
    const float fit=std::floor(std::max(0.0f,availableHeight-fixed)/rowH);
    return std::max(1.0f,std::min(rows,std::clamp(fit,2.0f,9.0f)))*rowH;
}
inline float fileAssocCardHeight(const UiMetrics&,float width,float availableHeight,const AppState& state) {
    return (width-36<620?306.0f:264.0f)+associationViewportHeight(width,availableHeight,state)+18.0f;
}
inline eui::Color associationColor(const EditorColors& colors, filetypes::Category category) {
    switch(category) {
        case filetypes::Category::Markdown: return colors.iconMd;
        case filetypes::Category::Text: return colors.iconTxt;
        case filetypes::Category::Code: return colors.iconCode;
        case filetypes::Category::Data: return colors.iconData;
        default: return colors.textMuted;
    }
}
inline void applyAssociationSelection(AppState& state) {
    const auto result=fileassoc::applySelection(state.associationSelected);
    showToast(state,result.ok?i18n::tr("settings.associations_updated"):i18n::tr("settings.associations_failed"),result.ok?i18n::tr("settings.associations_updated_hint"):result.error);
    refreshFileAssocState(state);
    state.associationSelected=fileassoc::registeredExtensions();
    app::requestUpdate();
}
inline void fileAssocCard(eui::Ui& ui,AppState& state,const EditorColors& colors,const UiMetrics& metrics,const char* uiFont,float y,float width,float availableHeight) {
    if(!state.associationSelectionLoaded) {
        state.associationSelected=fileassoc::registeredExtensions();
        if(state.associationSelected.empty() && !state.fileAssocEntriesPresent) state.associationSelected={"txt","md"};
        state.associationSelectionLoaded=true;
    }
    float& offset=ui.state<float>("settings.fileAssoc.offset");
    const float inner=std::max(0.0f,width-36), height=fileAssocCardHeight(metrics,width,availableHeight,state);
    const bool narrowButtons=inner<620;
    const float listY=y+(narrowButtons?306.0f:264.0f);
    const float rowH=associationRowHeight(inner), viewport=associationViewportHeight(width,availableHeight,state);
    const auto rows=associationRows(state);
    ui.rect("settings.fileAssoc.card").position(0,y).size(width,height).radius(8).color(colors.editor).border(1,colors.border).build();
    ui.text("settings.fileAssoc.label").position(18,y+16).size(inner,26).text(i18n::tr("assoc.title")).fontFamily(uiFont).fontSize(metrics.panelLabelFontSize).fontWeight(600).color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
    ui.text("settings.fileAssoc.description").position(18,y+48).size(inner,60).text(i18n::tr("settings.assoc_description"))
        .wrap(true).fontFamily(uiFont).fontSize(metrics.panelHintFontSize).color(colors.textMuted).verticalAlign(eui::VerticalAlign::Center).build();
    segmentedAt(ui,colors,"settings.fileAssoc.category",18,y+120,inner,
        {i18n::tr("assoc.all"),i18n::tr("assoc.text"),i18n::tr("assoc.data"),i18n::tr("assoc.code")},state.associationCategory,metrics.panelFontSize,[&state,&offset](int index){state.associationCategory=index;offset=0;app::requestUpdate();});
    // 一键注册/解除的入口：把当前过滤结果整体选上或撤选，再配合"应用更改"一次落地。
    const float selectW=92.0f;
    bool allSelected=!rows.empty();
    for(const auto* type:rows) if(std::find(state.associationSelected.begin(),state.associationSelected.end(),type->extension)==state.associationSelected.end()) {allSelected=false;break;}
    flatButton(ui,colors,"settings.fileAssoc.selectAll",i18n::tr(allSelected?"assoc.deselect_all":"assoc.select_all"),18+inner-selectW,y+164,selectW,32,false,metrics.panelFontSize,[&state,rows,allSelected] {
        auto& values=state.associationSelected;
        for(const auto* type:rows) {
            const auto found=std::find(values.begin(),values.end(),type->extension);
            if(allSelected) {if(found!=values.end()) values.erase(found);}
            else if(found==values.end()) values.push_back(type->extension);
        }
        app::requestUpdate();
    });
    components::input(ui,"settings.fileAssoc.search").position(18,y+164).size(std::max(120.0f,inner-selectW-8),32).value(state.associationSearch).placeholder(i18n::tr("assoc.search"))
        .fontFamily(uiFont).fontSize(metrics.panelFontSize).inset(8).theme(colors.tokens).onChange([&state,&offset](const std::string& value){state.associationSearch=value;offset=0;app::requestUpdate();}).build();
    const float buttonW=narrowButtons?(inner-8)/2:(inner-24)/4;
    auto button=[&](const char* id,const char* label,int index,bool accent,bool danger,std::function<void()> action) {
        flatButton(ui,colors,id,label,18+(index%(narrowButtons?2:4))*(buttonW+8),y+210+(index/(narrowButtons?2:4))*42,buttonW,34,accent,metrics.panelFontSize,std::move(action),danger);
    };
    button("settings.fileAssoc.register",i18n::tr(state.fileAssocEntriesPresent&&!state.fileAssocRegistered?"assoc.repair":"assoc.apply"),0,true,false,[&state] {
        bool removing=false;for(const auto& t:state.associationStatuses) if((t.registered || t.defaultApp==fileassoc::DefaultApp::EUIEdits) && std::find(state.associationSelected.begin(),state.associationSelected.end(),t.extension)==state.associationSelected.end()) removing=true;
        if(removing) requestRisk(state,i18n::tr("assoc.remove_some_title"),i18n::tr("assoc.remove_some_hint"),i18n::tr("assoc.apply"),[&state]{applyAssociationSelection(state);});
        else applyAssociationSelection(state);
    });
    button("settings.fileAssoc.defaults.open",i18n::tr("assoc.defaults"),1,false,false,[&state]{if(!fileassoc::openDefaultAppsSettings()) showToast(state,i18n::tr("assoc.settings_failed"),i18n::tr("assoc.settings_failed_hint"));});
    button("settings.fileAssoc.refresh",i18n::tr("assoc.refresh"),2,false,false,[&state]{refreshFileAssocState(state);state.associationSelected=fileassoc::registeredExtensions();app::requestUpdate();});
    button("settings.fileAssoc.remove",i18n::tr("assoc.clear"),3,false,true,[&state]{requestRisk(state,i18n::tr("assoc.clear_title"),i18n::tr("assoc.clear_hint"),i18n::tr("assoc.clear_confirm"),[&state]{unregisterDefaultFileType(state);state.associationSelected=fileassoc::registeredExtensions();app::requestUpdate();});});
    if(rows.empty()) {
        ui.text("settings.fileAssoc.empty").position(18,listY).size(inner,viewport).text(i18n::tr("assoc.empty"))
            .fontFamily(uiFont).fontSize(metrics.panelFontSize).color(colors.textMuted).verticalAlign(eui::VerticalAlign::Center).build();
        return;
    }
    offset=std::clamp(offset,0.0f,std::max(0.0f,rows.size()*rowH-viewport));
    components::virtualList(ui,"settings.fileAssoc.list").theme(colors.tokens).position(18,listY).size(inner,viewport)
        .itemCount(static_cast<std::int64_t>(rows.size())).rowHeight(rowH).offset(offset).step(rowH*2)
        .onChange([&offset](float value){offset=value;})
        .row([&state,&colors,&offset,rows,metrics,uiFont,rowH,viewport](eui::Ui& rowUi,const std::string&,std::int64_t index,float rowWidth,float) {
            if(index<0 || index>=static_cast<std::int64_t>(rows.size())) return;
            const auto& type=*rows[static_cast<std::size_t>(index)];
            const std::string ext=type.extension,id="settings.fileAssoc.type."+ext;
            const bool selected=std::find(state.associationSelected.begin(),state.associationSelected.end(),ext)!=state.associationSelected.end();
            fileassoc::TypeStatus status{ext};
            for(const auto& value:state.associationStatuses) if(value.extension==ext) {status=value;break;}
            const char* defaultLabel=status.defaultApp==fileassoc::DefaultApp::EUIEdits?"EUI-Edits":i18n::tr(status.defaultApp==fileassoc::DefaultApp::Other?"assoc.other":status.defaultApp==fileassoc::DefaultApp::None?"assoc.none":"assoc.unknown");
            const auto toggle=[&state,ext] {
                auto& values=state.associationSelected;const auto found=std::find(values.begin(),values.end(),ext);
                if(found==values.end()) values.push_back(ext);else values.erase(found);app::requestUpdate();
            };
            const bool compact=rowWidth<500;
            const float center=compact?20.0f:rowH*.5f, box=18.0f;
            const float pillWidth=std::max(58.0f,core::TextPrimitive::measureTextWidth("."+ext,uiFont,metrics.panelHintFontSize)+20.0f);
            const float statusW=compact?0.0f:std::min(220.0f,rowWidth*.38f);
            const float nameX=42.0f+pillWidth+10.0f;
            const bool focused=rowUi.isFocused(id+".hit");
            const eui::Color tint=associationColor(colors,type.category);
            rowUi.stack(id).size(rowWidth,rowH).content([&] {
                rowUi.rect(id+".hit").fill().radius(6).states(transparentColor(),colors.rowHover,colors.pressed)
                    .border(focused?1.5f:0.0f,colors.accent).focusable().onClick(toggle)
                    .onFocusChanged([](bool){app::requestUpdate();})
                    .onKeyEvent([toggle,&rowUi,&offset,rows,index,rowH,viewport](const eui::KeyEvent& event) {
                        if(!event.isDown()) return false;
                        if(event.key==eui::InputKey::Space || event.key==eui::InputKey::Enter) {
                            if(event.action==core::KeyAction::Press) toggle();
                            return true;
                        }
                        if(event.key==eui::InputKey::Tab || event.key==eui::InputKey::Up || event.key==eui::InputKey::Down) {
                            const auto direction=event.key==eui::InputKey::Up || (event.key==eui::InputKey::Tab && event.modifiers.shift)?-1:1;
                            const auto next=std::clamp<std::int64_t>(index+direction,0,static_cast<std::int64_t>(rows.size())-1);
                            if(event.key==eui::InputKey::Tab && next==index) {
                                rowUi.requestFocus("settings.fileAssoc.search.hit");app::requestUpdate();return true;
                            }
                            if(next*rowH<offset) offset=static_cast<float>(next)*rowH;
                            else if((next+1)*rowH>offset+viewport) offset=static_cast<float>(next+1)*rowH-viewport;
                            rowUi.requestFocus("settings.fileAssoc.type."+std::string(rows[static_cast<std::size_t>(next)]->extension)+".hit");
                            app::requestUpdate();return true;
                        }
                        return false;
                    }).build();
                rowUi.rect(id+".checkbox").position(10,center-box*.5f).size(box,box).radius(4)
                    .color(selected?colors.accent:colors.editor).border(selected?0.0f:1.5f,colors.textMuted).build();
                if(selected) components::vector_icon::drawCheckmark(rowUi,id+".check",12,center-7,14,rgba(1,1,1,1));
                rowUi.rect(id+".pill").position(42,center-12).size(pillWidth,24).radius(12)
                    .color(rgba(tint.r,tint.g,tint.b,.12f)).border(1,rgba(tint.r,tint.g,tint.b,.24f)).build();
                rowUi.text(id+".extension").position(42,center-12).size(pillWidth,24).text("."+ext)
                    .fontFamily(uiFont).fontSize(metrics.panelHintFontSize).fontWeight(600).color(tint)
                    .horizontalAlign(eui::HorizontalAlign::Center).verticalAlign(eui::VerticalAlign::Center).build();
                rowUi.text(id+".language").position(nameX,center-12).size(std::max(0.0f,rowWidth-nameX-statusW-10),24)
                    .text(elideToWidth(filetypes::label(type.language),std::max(0.0f,rowWidth-nameX-statusW-10),metrics.panelFontSize))
                    .fontFamily(uiFont).fontSize(metrics.panelFontSize).color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
                const std::string statusText=i18n::format("assoc.state",{{"added",i18n::tr(status.registered?"assoc.added":"assoc.not_added")},{"default",defaultLabel}});
                rowUi.text(id+".state").position(compact?42:rowWidth-statusW,compact?37:5)
                    .size(compact?std::max(0.0f,rowWidth-54):statusW-8,compact?20:rowH-10).text(statusText)
                    .wrap(true).fontFamily(uiFont).fontSize(metrics.panelHintFontSize).color(colors.textMuted).verticalAlign(eui::VerticalAlign::Center).build();
                rowUi.rect(id+".rule").position(10,rowH-1).size(std::max(0.0f,rowWidth-20),1).color(rgba(colors.border.r,colors.border.g,colors.border.b,.35f)).build();
            }).build();
        }).build();
}

inline void chooseFontFile(AppState& state, FontPickerTarget target) {
    const std::string& currentPath =
        target == FontPickerTarget::Editor ? state.editorFontFile
        : target == FontPickerTarget::Code ? state.codeFontFile
                                           : state.uiFontFile;
    eui::platform::FileDialogOptions options;
    options.prompt = i18n::tr("settings.font_import_file");
    options.allowedExtensions = {"ttf", "otf", "ttc"};
    options.filterName = i18n::tr("settings.font_file");
    if (!currentPath.empty()) {
        options.initialDirectory = textfile::parentPath(currentPath);
    }

    const eui::platform::FileDialogResult result = eui::platform::openFileDialog(options);
    if (result.status == eui::platform::FileDialogStatus::Cancelled) {
        return;
    }
    if (!result.selected()) {
        showToast(state, i18n::tr("settings.font_select_failed"),
                  result.error.empty() ? i18n::tr("settings.dialog_no_file") : result.error);
        return;
    }

    // apply*FontFile 负责安全探测、代码字面的等宽校验、保护记录与持久化。
    const std::string& path = result.paths.front();
    if (target == FontPickerTarget::Editor) {
        applyEditorFontFile(state, path);
    } else if (target == FontPickerTarget::Code) {
        applyCodeFontFile(state, path);
    } else {
        applyUiFontFile(state, path);
    }
}

// 面板底部的一对按钮：左边次要（清除/恢复默认），右边主操作（关闭/返回）。
inline void footerButtons(eui::Ui& ui,
                          const EditorColors& colors,
                          const UiMetrics& metrics,
                          const PanelGeometry& geometry,
                          const std::string& leftId,
                          const std::string& leftLabel,
                          std::function<void()> onLeft,
                          const std::string& rightLabel,
                          std::function<void()> onRight) {
    ui.rect("settings.footer.rule")
        .x(kPadding)
        .y(geometry.footerRuleY)
        .size(geometry.contentWidth, 1.0f)
        .color(colors.border)
        .build();
    flatButton(ui, colors, leftId, leftLabel, kPadding, geometry.buttonY,
               kButtonWidth, kButtonHeight, false, metrics.panelFontSize, std::move(onLeft));
    flatButton(ui, colors, "settings.footer.primary", rightLabel,
               kPadding + geometry.contentWidth - kButtonWidth, geometry.buttonY,
               kButtonWidth, kButtonHeight, true, metrics.panelFontSize, std::move(onRight));
}

// 字体选择页。行高固定（virtualList 的前提），列表里只画字体名、
// 不按字体自身渲染每一行——那会把几十个字体文件全塞进字体栈与字形图集，
// 图集填满后是静默不画（见 反馈调研 §2.7）。预览只给"当前选中"这一个字体做一份。
inline void fontPickerPage(eui::Ui& ui,
                           AppState& state,
                           const EditorColors& colors,
                           const UiMetrics& metrics,
                           const PanelGeometry& availableGeometry) {
    PanelGeometry geometry = availableGeometry;
    geometry.footerRuleY += 32.0f;
    geometry.buttonY = geometry.footerRuleY + 8.0f;
    const float targetY = 12.0f;
    const float currentY = targetY + 38.0f;
    const FontPickerTarget target = state.fontPicker;
    const std::string& currentFile =
        target == FontPickerTarget::Editor ? state.editorFontFile
        : target == FontPickerTarget::Code ? state.codeFontFile
                                           : state.uiFontFile;
    const std::vector<fonts::Entry>& entries = fonts::catalog();

    segmentedAt(ui, colors, "settings.fontPicker.target",
                kPadding, targetY, std::min(geometry.contentWidth, 340.0f), {i18n::tr("settings.font_body"), i18n::tr("settings.font_code"), i18n::tr("settings.font_interface")},
                target == FontPickerTarget::Editor ? 0
                : target == FontPickerTarget::Code ? 1
                                                   : 2,
                metrics.panelFontSize,
                [&state](int index) {
                    state.fontPicker = index == 1 ? FontPickerTarget::Code
                                       : index == 2 ? FontPickerTarget::Ui
                                                    : FontPickerTarget::Editor;
                    app::requestUpdate();
                });

    const std::string currentName = target == FontPickerTarget::Code && currentFile.empty()
        ? std::string(i18n::tr("settings.font_mono_preset"))
        : currentFile.empty() ? std::string(i18n::tr("settings.font_preset")) : fonts::displayNameFor(currentFile);
    const std::string currentNotes =
        std::string(target == FontPickerTarget::Code ? i18n::tr("settings.font_mono_note") : "") +
        (!currentFile.empty() && textfile::extensionLower(currentFile) == "ttc"
             ? i18n::tr("settings.font_collection_note") : "");
    const float importButtonWidth = std::min(124.0f, geometry.contentWidth * 0.38f);
    ui.text("settings.fontPicker.current")
        .x(kPadding)
        .y(currentY)
        .size(std::max(0.0f, geometry.contentWidth - importButtonWidth - 10.0f), kControlHeight)
        .text(elideToWidth(i18n::format("settings.font_current_value",{{"name",currentName},{"notes",currentNotes}}),
                           std::max(0.0f, geometry.contentWidth - importButtonWidth - 10.0f),
                           metrics.panelHintFontSize))
        .fontFamily(uiFontFamily(state))
        .fontSize(metrics.panelHintFontSize)
        .color(colors.textMuted)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
    flatButton(ui, colors, "settings.fontPicker.import", i18n::tr("settings.font_import"),
               kPadding + geometry.contentWidth - importButtonWidth, currentY,
               importButtonWidth, kControlHeight, false, metrics.panelHintFontSize,
               [&state, target] { chooseFontFile(state, target); });

    // 预览只使用当前字体，不让每个列表行都加载各自的字体文件。
    const float sampleY = currentY + 38.0f;
    const float sampleFontSize = metrics.panelFontSize + 3.0f;
    const float sampleLineHeight = std::round(sampleFontSize * 1.4f);
    const bool codePreview = target == FontPickerTarget::Code;
    const float requestedSampleHeight = 40.0f + sampleLineHeight * (codePreview ? 3 : 1) + 12;
    const float previewSpace = std::max(0.0f, geometry.footerRuleY - sampleY - 90);
    const bool showSample = requestedSampleHeight <= previewSpace;
    const float sampleHeight = showSample ? requestedSampleHeight : 0;
    const std::string sampleText = codePreview
        ? "const item  = 42;\nconst total = item * 2;\nreturn total;"
        : "永和九年 岁在癸丑 Hello 123";
    if (showSample) {
        ui.rect("settings.fontPicker.preview.card").position(kPadding, sampleY)
            .size(geometry.contentWidth, sampleHeight).radius(8)
            .color(colors.toolbar).border(1, colors.border).build();
        ui.text("settings.fontPicker.preview.label").position(kPadding+14, sampleY+10)
            .size(geometry.contentWidth-28, 24).text(i18n::tr("settings.font_preview"))
            .fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize)
            .color(colors.textMuted).build();
        ui.text("settings.fontPicker.sample").position(kPadding+14, sampleY+38)
            .size(geometry.contentWidth-28, sampleHeight-46).text(sampleText)
            .fontFamily(target == FontPickerTarget::Editor ? editorFontFamily(state)
                        : target == FontPickerTarget::Code ? codeFontFamily(state) : uiFontFamily(state))
            .fontSize(sampleFontSize).lineHeight(sampleLineHeight).color(colors.text).build();
    }
    const float listY = sampleY + sampleHeight + (showSample ? 14 : 0);
    const float listHeight = std::max(0.0f, geometry.footerRuleY - 12 - listY);
    const float rowHeight = std::max(36.0f, metrics.panelFontSize + 22);
    ui.rect("settings.fontPicker.list.card").position(kPadding, listY)
        .size(geometry.contentWidth, listHeight).radius(8)
        .color(colors.editor).border(1, colors.border).build();

    std::vector<const fonts::Entry*> visibleEntries;
    fonts::Entry selectedOutsideCatalog;
    bool selectedPathInCatalog = false;
    for (const fonts::Entry& entry : entries) {
        const bool selected = !currentFile.empty() && entry.path == currentFile;
        selectedPathInCatalog = selectedPathInCatalog || selected;
        if (target != FontPickerTarget::Code || entry.monospaceHint || selected) {
            visibleEntries.push_back(&entry);
        }
    }
    if (target == FontPickerTarget::Code && !currentFile.empty() && !selectedPathInCatalog) {
        selectedOutsideCatalog.displayName = fonts::displayNameFor(currentFile);
        selectedOutsideCatalog.path = currentFile;
        visibleEntries.push_back(&selectedOutsideCatalog);
    }

    if (listHeight < rowHeight) {
        ui.text("settings.fontPicker.tooShort")
            .x(kPadding).y(listY).size(geometry.contentWidth, std::max(0.0f, listHeight))
            .text(i18n::tr("settings.font_low_height"))
            .fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize)
            .color(colors.textMuted).wrap(true).build();
    } else if (visibleEntries.empty()) {
        ui.text("settings.fontPicker.empty")
            .x(kPadding)
            .y(listY)
            .size(geometry.contentWidth, listHeight)
            .text(target == FontPickerTarget::Code
                      ? i18n::tr("settings.font_no_mono")
                      : i18n::tr("settings.font_no_files"))
            .fontFamily(uiFontFamily(state))
            .fontSize(metrics.panelFontSize)
            .color(colors.textMuted)
            .wrap(true)
            .build();
    } else {
        components::virtualList(ui, "settings.fontPicker.list")
            .theme(colors.tokens)
            .position(kPadding, listY)
            .size(geometry.contentWidth, listHeight)
            .itemCount(static_cast<std::int64_t>(visibleEntries.size()))
            .rowHeight(rowHeight)
            .offset(state.fontListScroll)
            .step(rowHeight * 3.0f)
            .overscanViewports(0.5f)
            .transition(quickTransition())
            .onChange([&state](float value) { state.fontListScroll = value; })
            .row([&state, &colors, &visibleEntries, currentFile, target, rowHeight, metrics](
                     eui::Ui& rowUi, const std::string& rowId, std::int64_t index, float rowWidth, float) {
                const auto rowIndex = static_cast<std::size_t>(index);
                if (rowIndex >= visibleEntries.size()) {
                    return;
                }
                const fonts::Entry& entry = *visibleEntries[rowIndex];
                const bool active = !currentFile.empty() && entry.path == currentFile;
                const eui::Color normal = active ? colors.rowActive : colors.editor;
                const eui::Color hover = active ? colors.rowActive : colors.rowHover;

                rowUi.stack(rowId)
                    .size(rowWidth, rowHeight)
                    .content([&] {
                        rowUi.rect(rowId + ".bg")
                            .fill()
                            .radius(4.0f)
                            .states(normal, hover, colors.pressed)
                            .transition(quickTransition())
                            .cursor(eui::CursorShape::Hand)
                            // 不让点设置面板把焦点从编辑区抢走。
                            .preserveFocusOnPress()
                            .onClick([&state, entry, target] {
                                if (target == FontPickerTarget::Editor) {
                                    applyEditorFontFile(state, entry.path);
                                } else if (target == FontPickerTarget::Code) {
                                    applyCodeFontFile(state, entry.path);
                                } else {
                                    applyUiFontFile(state, entry.path);
                                }
                            })
                            .build();

                        if (active) {
                            rowUi.rect(rowId + ".mark")
                                .position(1.0f, 3.0f)
                                .size(2.0f, std::max(0.0f, rowHeight - 6.0f))
                                .radius(1.0f)
                                .color(colors.accent)
                                .build();
                        }

                        rowUi.rect(rowId+".rule").position(10, rowHeight-1)
                            .size(std::max(0.0f, rowWidth-20), 1).color(colors.border).build();
                        std::vector<std::string> tags;
                        if (active) tags.push_back(i18n::tr("settings.font_current"));
                        if (entry.monospaceHint) tags.push_back(i18n::tr("settings.font_mono"));
                        if (entry.italic) tags.push_back(i18n::tr("settings.font_italic"));
                        if (textfile::extensionLower(entry.path) == "ttc") tags.push_back(i18n::tr("settings.font_ttc_first"));
                        float right = rowWidth-14;
                        const float tagFont = metrics.panelHintFontSize;
                        for (std::size_t t=0; t<tags.size(); ++t) {
                            const float tagWidth = core::TextPrimitive::measureTextWidth(tags[t], uiFontFamily(state), tagFont)+16;
                            if (right-tagWidth < rowWidth*.38f) break;
                            right -= tagWidth;
                            const std::string tagId = rowId+".tag."+std::to_string(t);
                            rowUi.rect(tagId+".bg").position(right, (rowHeight-24)*.5f).size(tagWidth, 24)
                                .radius(4).color(colors.toolbar).border(1, colors.border).build();
                            rowUi.text(tagId+".text").position(right, (rowHeight-24)*.5f).size(tagWidth, 24)
                                .text(tags[t]).fontFamily(uiFontFamily(state)).fontSize(tagFont)
                                .color(colors.textMuted).horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center).build();
                            right -= 6;
                        }
                        rowUi.text(rowId + ".name").position(14, 0)
                            .size(std::max(0.0f, right-28), rowHeight)
                            .text(elideToWidth(entry.displayName, std::max(0.0f, right-28), metrics.panelFontSize))
                            .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize)
                            .color(active ? colors.accent : colors.text)
                            .verticalAlign(eui::VerticalAlign::Center).build();
                    })
                    .build();
            })
            .build();
    }

    footerButtons(ui, colors, metrics, geometry,
                  "settings.fontPicker.reset",
                  i18n::tr("settings.font_use_preset"),
                  [&state, target] {
                      requestRisk(state,i18n::tr("settings.font_reset_title"),i18n::tr("settings.font_reset_hint"),i18n::tr("settings.font_reset_confirm"),[&state,target] {
                      if (target == FontPickerTarget::Editor) {
                          applyEditorFontFile(state, std::string{});
                      } else if (target == FontPickerTarget::Code) {
                          applyCodeFontFile(state, std::string{});
                      } else {
                          applyUiFontFile(state, std::string{});
                      }
                      });
                  },
                  i18n::tr("settings.back"),
                  [&state] {
                      state.fontPicker = FontPickerTarget::None;
                      app::requestUpdate();
                  });
}

inline void themeListPage(eui::Ui& ui, AppState& state, const EditorColors& colors,
                          const UiMetrics& metrics, const PanelGeometry& geometry) {
    ui.text("settings.themeList.title")
        .x(kPadding).y(18.0f).size(geometry.contentWidth, 32.0f)
        .text(i18n::tr("settings.theme_library"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize + 3.0f)
        .color(colors.text).build();
    ui.rect("settings.themeList.rule")
        .x(kPadding).y(54.0f).size(geometry.contentWidth, 1.0f)
        .color(colors.border).build();
    ui.text("settings.themeList.hint")
        .x(kPadding).y(64.0f).size(geometry.contentWidth, 27.0f)
        .text(i18n::tr("settings.theme_library_hint"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize)
        .color(colors.textMuted).build();

    const auto& entries = themeCatalog();
    const float listY = 96.0f;
    const float listHeight = std::max(64.0f, geometry.footerRuleY - 12.0f - listY);
    const float rowHeight = std::max(28.0f, std::round(metrics.panelFontSize * 1.9f));
    if (entries.empty() && state.themeFile.empty()) {
        ui.text("settings.themeList.empty")
            .x(kPadding).y(listY).size(geometry.contentWidth, listHeight)
            .text(i18n::tr("settings.theme_library_empty"))
            .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize)
            .color(colors.textMuted).wrap(true).build();
    } else {
        // 索引 0 固定为"内置配色"：清掉主题文件回到默认。其余行是主题库目录。
        constexpr std::int64_t kBuiltinRows = 1;
        components::virtualList(ui, "settings.themeList.list")
            .theme(colors.tokens)
            .position(kPadding, listY)
            .size(geometry.contentWidth, listHeight)
            .itemCount(static_cast<std::int64_t>(entries.size()) + kBuiltinRows)
            .rowHeight(rowHeight)
            .offset(state.themeListScroll)
            .step(rowHeight * 3.0f)
            .overscanViewports(0.5f)
            .transition(quickTransition())
            .onChange([&state](float value) { state.themeListScroll = value; })
            .row([&state, &colors, &entries, rowHeight](eui::Ui& rowUi,
                   const std::string& rowId, std::int64_t index, float rowWidth, float) {
                const std::size_t i = static_cast<std::size_t>(index);
                const bool builtinRow = index < 1;
                if (!builtinRow && i - 1 >= entries.size()) return;
                const ThemeCatalogEntry entry = builtinRow ? ThemeCatalogEntry{} : entries[i - 1];
                const bool active = builtinRow ? state.themeFile.empty() : entry.path == state.themeFile;
                rowUi.stack(rowId).size(rowWidth, rowHeight).content([&] {
                    rowUi.rect(rowId + ".bg")
                        .fill().radius(4.0f)
                        .states(active ? colors.rowActive : transparentColor(),
                                active ? colors.rowActive : colors.rowHover, colors.pressed)
                        .transition(quickTransition()).cursor(eui::CursorShape::Hand)
                        .preserveFocusOnPress()
                        .onClick([&state, builtinRow, entry] {
                            if (builtinRow) {
                                // 内置配色与列表项一样点一下就生效；主题文件还在库里，随时可以点回来。
                                resetThemeFile(state);
                                state.themeListOpen = false;
                            } else if (applyThemeFilePath(state, entry.path)) {
                                state.themeListOpen = false;
                            }
                        }).build();
                    rowUi.text(rowId + ".name")
                        .position(10.0f, 0.0f)
                        .size(std::max(0.0f, rowWidth - 20.0f), rowHeight)
                        .text(builtinRow ? std::string(i18n::tr("settings.theme_builtin_entry")) : entry.name)
                        .fontFamily(uiFontFamily(state))
                        .fontSize(std::round(std::max(11.0f, rowHeight * 0.58f)))
                        .color(active ? colors.accent : colors.text)
                        .verticalAlign(eui::VerticalAlign::Center).build();
                }).build();
            }).build();
    }
    footerButtons(ui, colors, metrics, geometry,
                  "settings.themeList.file", i18n::tr("settings.pick_file"),
                  [&state] { chooseThemeFile(state); },
                  i18n::tr("settings.back"), [&state] { state.themeListOpen = false; app::requestUpdate(); });
}


} // namespace settings_detail

inline void settingsPanelOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    using namespace settings_detail;
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    const char* uiFont = uiFontFamily(state);

    if (state.settingsOpen && !state.settingsOpenLast) {
        state.fontPicker = FontPickerTarget::None;
        state.themeListOpen = false;
        refreshFileAssocState(state);
        app::requestUpdate();
    }
    state.settingsOpenLast = state.settingsOpen;
    if (!state.settingsOpen) return;

    const float headerHeight = std::max(72.0f, metrics.pageTitleFontSize + 36.0f);
    const bool compactNav = screen.width < 840.0f;
    const float navWidth = compactNav ? 0.0f : 176.0f;
    const float pageTop = headerHeight + (compactNav ? 54.0f : 0.0f);
    const float remainingWidth = std::max(0.0f, screen.width - navWidth);
    const float pageWidth = std::min(952.0f, remainingWidth);
    const float pageX = navWidth + (remainingWidth - pageWidth) * 0.5f;
    const float pageHeight = std::max(0.0f, screen.height - pageTop);
    const PanelGeometry geometry = [&] {
        PanelGeometry g;
        g.contentWidth = std::max(0.0f, pageWidth - kPadding * 2.0f);
        g.controlWidth = std::min(std::max(0.0f, g.contentWidth - 36.0f),
                                  std::max(224.0f, metrics.panelFontSize * 16.0f));
        g.stacked = g.contentWidth < g.controlWidth + metrics.panelFontSize * 21.0f + 60.0f;
        if (g.stacked) g.controlWidth = std::max(0.0f, g.contentWidth - 36.0f);
        g.controlX = g.stacked ? 18.0f : g.contentWidth - g.controlWidth - 18.0f;
        g.labelWidth = std::max(0.0f, g.stacked ? g.contentWidth - 36.0f : g.controlX - 42.0f);
        // 卡片压矮约四分之一（顶部/间距/底部留白见 rowLabel：文字块按实测说明行数
        // 在卡片内竖向居中，这里只按两行说明的满额高度定卡片高度）。
        const float labelTop = 14.0f;
        const float textBottom = labelTop + metrics.panelLabelFontSize + 4.0f +
                                 (metrics.panelHintFontSize + 5.0f)*2.0f;
        g.cardHeight = textBottom + 6.0f + (g.stacked ? 14.0f + kControlHeight : 0.0f);
        g.controlTop = g.stacked ? textBottom + 14.0f : (g.cardHeight-kControlHeight)*.5f;
        g.rowHeight = g.cardHeight + 12.0f;
        g.footerRuleY = std::max(0.0f, pageHeight - 86.0f);
        g.buttonY = g.footerRuleY + 8.0f;
        return g;
    }();
    const float controlWidth = geometry.controlWidth;
    const float controlX = geometry.controlX;
    const float labelWidth = geometry.labelWidth;
    const float hintRowControlY = geometry.controlTop;
    const float plainRowControlY = hintRowControlY;

    ui.stack("settings.page").size(screen.width, screen.height).zIndex(800).content([&] {
        ui.rect("settings.page.bg").fill().color(colors.window).build();
        ui.rect("settings.page.header")
            .size(screen.width, headerHeight).color(colors.toolbar).build();
        if (!compactNav) {
            ui.rect("settings.page.navBg")
                .position(0.0f, headerHeight).size(navWidth, pageHeight)
                .color(colors.panel).build();
        }

        ui.stack("settings.backToEditor.slot").position(18, (headerHeight-34)*.5f).size(154, 34).content([&] {
            iconTextButton(ui, "settings.backToEditor", UiIcon::ArrowLeft, i18n::tr("settings.back_editor"), 154,
                           metrics.panelFontSize, [&state] {
                               state.settingsOpen = false;
                               state.fontPicker = FontPickerTarget::None;
                               state.themeListOpen = false;
                               app::requestUpdate();
                           }, uiFont);
        }).build();
        ui.text("settings.page.title")
            .position(194.0f, 0.0f).size(std::max(0.0f, screen.width - 208.0f), headerHeight)
            .text(state.fontPicker != FontPickerTarget::None ? i18n::tr("settings.choose_font")
                  : state.themeListOpen ? i18n::tr("settings.theme_library") : i18n::tr("settings.title"))
            .fontFamily(uiFont).fontSize(metrics.pageTitleFontSize).fontWeight(600)
            .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();

        const std::vector<std::pair<SettingsCategory, const char*>> categories = {
            {SettingsCategory::Appearance, i18n::tr("settings.appearance")},
            {SettingsCategory::Editor, i18n::tr("settings.editor")},
            {SettingsCategory::FileSystem, i18n::tr("settings.files_system")},
            {SettingsCategory::About, i18n::tr("settings.about_nav")},
        };
        const float navSlots = static_cast<float>(categories.size());
        float navY = headerHeight + (compactNav ? 7.0f : 20.0f);
        std::size_t categoryIndex = 0;
        for (const auto& entry : categories) {
            const bool active = state.settingsCategory == entry.first;
            flatButton(ui, colors, std::string("settings.nav.") + std::to_string(static_cast<int>(entry.first)), entry.second,
                       compactNav ? 12.0f + categoryIndex * ((screen.width - 24.0f) / navSlots) : 12.0f,
                       navY, compactNav ? (screen.width - 24.0f) / navSlots - 4.0f : navWidth - 24.0f, 40.0f,
                       active, metrics.panelFontSize,
                        [&state, category = entry.first] {
                            state.settingsCategory = category;
                            state.settingsScroll = 0.0f;
                            state.fontPicker = FontPickerTarget::None;
                            state.themeListOpen = false;
                            if (category == SettingsCategory::FileSystem) {
                                refreshFileAssocState(state);
                            }
                            app::requestUpdate();
                       });
            if (!compactNav) navY += 48.0f;
            ++categoryIndex;
        }

        ui.stack("settings.contentPane")
            .position(pageX, pageTop).size(pageWidth, pageHeight)
            .content([&] {
                if (state.themeListOpen) {
                    themeListPage(ui, state, colors, metrics, geometry);
                    return;
                }
                if (state.fontPicker != FontPickerTarget::None) {
                    fontPickerPage(ui, state, colors, metrics, geometry);
                    return;
                }
                if (state.settingsCategory == SettingsCategory::About) {
                    aboutPage(ui, state, colors, metrics, geometry.contentWidth, kPadding, pageHeight);
                    return;
                }

                const float scrollTop = 24.0f;
                const float scrollHeight = std::max(0.0f, pageHeight - scrollTop - 18.0f);
                const float rowHeight = geometry.rowHeight;
                const float sectionHeight = metrics.sectionTitleFontSize + 26.0f;
                const float associationSpace = std::max(0.0f, scrollHeight-sectionHeight-rowHeight);
                int rowCount = state.settingsCategory == SettingsCategory::Appearance ? 7
                             : state.settingsCategory == SettingsCategory::Editor ? 4 : 1;
                // 行数之外只剩"恢复默认值"一行，所以 +1。
                const float contentHeight = sectionHeight + rowHeight * (rowCount+1) + 10.0f +
                    (state.settingsCategory == SettingsCategory::FileSystem
                        ? fileAssocCardHeight(metrics, geometry.contentWidth, associationSpace, state) + 12.0f : 0.0f);
                const float maxOffset = std::max(0.0f, contentHeight - scrollHeight);
                const float scrollStep = std::max(20.0f, std::min(rowHeight*.5f, scrollHeight*.45f));
                state.settingsScroll = std::clamp(state.settingsScroll, 0.0f, maxOffset);
                const std::string scrollId = "settings.main.scroll." +
                    std::to_string(static_cast<int>(state.settingsCategory));
                ui.stack(scrollId)
                    .position(kPadding, scrollTop)
                    .size(geometry.contentWidth, scrollHeight)
                    .clip()
                    .scrollState(scrollId, state.settingsScroll, maxOffset, scrollStep)
                    .onScrollOffsetChanged([&state](float value) { state.settingsScroll = value; })
                    .content([&] {
                        ui.stack("settings.main.content")
                            .size(geometry.contentWidth, contentHeight)
                            .scrollContentFrom(scrollId)
                            .content([&] {
                                ui.text("settings.section.title")
                                    .position(0.0f, 0.0f).size(geometry.contentWidth, sectionHeight-12.0f)
                                    .text(state.settingsCategory == SettingsCategory::Appearance ? i18n::tr("settings.appearance")
                                          : state.settingsCategory == SettingsCategory::Editor ? i18n::tr("settings.editor")
                                                                                              : i18n::tr("settings.files_system"))
                                    .fontFamily(uiFont).fontSize(metrics.sectionTitleFontSize).fontWeight(600)
                                    .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
                                float rowY = sectionHeight;
                                if (state.settingsCategory == SettingsCategory::Editor) {
            rowLabel(ui, colors, metrics, uiFont, "settings.fontSize", i18n::tr("settings.editor_font_size"), i18n::tr("settings.editor_font_size_hint"),
                     0.0f, rowY, labelWidth, geometry);
            stepControl(ui, colors, "settings.fontSize",
                        controlX, rowY + hintRowControlY, controlWidth,
                        std::to_string(static_cast<int>(std::lround(state.editorFontSize))),
                        state.editorFontSize > kMinimumEditorFontSize + 0.001f,
                        state.editorFontSize < kMaximumEditorFontSize - 0.001f,
                        [&state] { applyEditorFontSize(state, state.editorFontSize - 1.0f); },
                        [&state] { applyEditorFontSize(state, state.editorFontSize + 1.0f); },
                        uiFont,
                        metrics.panelFontSize);
            rowY += geometry.rowHeight;

                                }
                                if (state.settingsCategory == SettingsCategory::Appearance) {
            rowLabel(ui,colors,metrics,uiFont,"settings.uiLanguage",i18n::tr("menu.ui_language"),i18n::tr("settings.language_hint"),0,rowY,labelWidth,geometry);
            segmentedAt(ui,colors,"settings.uiLanguage",controlX,rowY+plainRowControlY,controlWidth,
                {i18n::tr("settings.language_auto"),"简体中文","English"},i18n::preference()=="zh-CN"?1:i18n::preference()=="en"?2:0,metrics.panelFontSize,
                [&state](int index){applyUiLanguage(state,index==1?"zh-CN":index==2?"en":"system");});
            rowY+=geometry.rowHeight;

            rowLabel(ui, colors, metrics, uiFont, "settings.theme", i18n::tr("settings.appearance"),
                     i18n::tr("settings.appearance_hint"),
                     0.0f, rowY, labelWidth, geometry);
            segmentedAt(ui, colors, "settings.theme",
                        controlX, rowY + plainRowControlY, controlWidth,
                        {i18n::tr("settings.theme_system"), i18n::tr("settings.dark"), i18n::tr("settings.light")},
                        state.themeFollowSystem ? 0 : (state.theme == ThemeMode::Light ? 2 : 1), metrics.panelFontSize,
                        [&state](int index) {
                            if (index == 0) {
                                applyFollowSystemTheme(state);
                            } else {
                                applyTheme(state, index == 2 ? ThemeMode::Light : ThemeMode::Dark);
                            }
                        });
            rowY += geometry.rowHeight;

            rowLabel(ui, colors, metrics, uiFont, "settings.scale", i18n::tr("settings.ui_scale"), systemScaleHint(state),
                     0.0f, rowY, labelWidth, geometry);
            stepControl(ui, colors, "settings.scale",
                        controlX, rowY + hintRowControlY, controlWidth,
                        scaleText(state),
                        state.uiScale > kMinimumUiScale + 0.001f,
                        state.uiScale < kMaximumUiScale - 0.001f,
                        [&state] { applyUiScale(state, state.uiScale - kUiScaleStep); },
                        [&state] { applyUiScale(state, state.uiScale + kUiScaleStep); },
                        uiFont,
                        metrics.panelFontSize);
            rowY += geometry.rowHeight;

            rowLabel(ui, colors, metrics, uiFont, "settings.uiFontSize", i18n::tr("settings.ui_font_size"),
                     i18n::tr("settings.ui_font_size_hint"),
                     0.0f, rowY, labelWidth, geometry);
            stepControl(ui, colors, "settings.uiFontSize",
                        controlX, rowY + hintRowControlY, controlWidth,
                        std::to_string(static_cast<int>(std::lround(state.uiFontSize))),
                        state.uiFontSize > kMinimumUiFontSize + 0.001f,
                        state.uiFontSize < kMaximumUiFontSize - 0.001f,
                        [&state] { applyUiFontSize(state, state.uiFontSize - 1.0f); },
                        [&state] { applyUiFontSize(state, state.uiFontSize + 1.0f); },
                        uiFont,
                        metrics.panelFontSize);
            rowY += geometry.rowHeight;

            // 字体：打开字体选择子页（正文/代码/界面三个目标共用）。槽按钮一行的
            // 宽度放不下三份状态，之前只显示正文一项容易让人误会——按钮只说
            // "点这里去选"，三个目标各自的当前值写在左侧说明里。
            const std::string fontsCurrent = i18n::format("settings.fonts_current",{
                {"body", state.editorFontFile.empty() ? std::string(i18n::tr("settings.font_default_short"))
                                                      : fonts::displayNameFor(state.editorFontFile)},
                {"code", state.codeFontFile.empty() ? std::string(i18n::tr("settings.font_default_short"))
                                                    : fonts::displayNameFor(state.codeFontFile)},
                {"ui", state.uiFontFile.empty() ? std::string(i18n::tr("settings.font_default_short"))
                                                : fonts::displayNameFor(state.uiFontFile)}});
            rowLabel(ui, colors, metrics, uiFont, "settings.fonts", i18n::tr("settings.fonts"),
                     fontsCurrent,
                     0.0f, rowY, labelWidth, geometry);
            slotButton(ui, colors, uiFont, metrics, "settings.fonts",
                       i18n::tr("settings.fonts_action"),
                       controlX, rowY + hintRowControlY, controlWidth,
                       !state.editorFontFile.empty() || !state.codeFontFile.empty() ||
                           !state.uiFontFile.empty(),
                       [&state] {
                           state.fontPicker = FontPickerTarget::Editor;
                           state.fontListScroll = 0.0f;
                           app::requestUpdate();
                       });
            rowY += geometry.rowHeight;

            // 主题文件（T13 第一阶段）：上面"外观"切的是内置两套配色，这行通向
            // 主题库子页（载入外部 theme.json / Obsidian 主题的覆盖表，见
            // model/theme_loader.h）。应用与"回到内置配色"都在库页里完成，
            // 行内不再放单独的重置按钮。
            rowLabel(ui, colors, metrics, uiFont, "settings.themeFile", i18n::tr("settings.theme_file"),
                     state.themeFile.empty()
                         ? std::string(i18n::tr("settings.theme_builtin_hint"))
                         : elideToWidth(activeTheme().version >= themeloader::kSchemaVersion2
                                            ? i18n::format("settings.theme_counts_compact",{
                                                {"name",activeTheme().name},{"light",std::to_string(activeTheme().light.colors.size())},
                                                {"dark",std::to_string(activeTheme().dark.colors.size())}})
                                            : textfile::fileName(state.themeFile),
                                        labelWidth, metrics.panelHintFontSize),
                     0.0f, rowY, labelWidth, geometry);
            slotButton(ui, colors, uiFont, metrics, "settings.themeFile", i18n::tr("settings.theme_open"),
                       controlX, rowY + hintRowControlY, controlWidth, !state.themeFile.empty(),
                       [&state] { openThemeChooser(state); });
            rowY += geometry.rowHeight;

            // 交互反馈动画（悬停/按压配色过渡与按压缩放）。关掉后界面即时响应、
            // 不再产生过渡帧，是给性能敏感场景留的出口；默认跟随系统"动画效果"。
            rowLabel(ui, colors, metrics, uiFont, "settings.animations", i18n::tr("settings.animations"),
                     i18n::tr("settings.animations_hint"),
                     0.0f, rowY, labelWidth, geometry);
            segmentedAt(ui, colors, "settings.animations",
                        controlX, rowY + plainRowControlY, controlWidth,
                        {i18n::tr("settings.on"), i18n::tr("settings.off")}, state.animations ? 0 : 1, metrics.panelFontSize,
                        [&state](int index) {
                            state.animations = index == 0;
                            persistSettings(state);
                            app::requestUpdate();
                        });
            rowY += geometry.rowHeight;
                                }
                                if (state.settingsCategory == SettingsCategory::FileSystem) {
            rowLabel(ui, colors, metrics, uiFont, "settings.mode", i18n::tr("settings.layout"), i18n::tr("settings.layout_hint"),
                     0.0f, rowY, labelWidth, geometry);
            segmentedAt(ui, colors, "settings.mode",
                        controlX, rowY + plainRowControlY, controlWidth,
                        {i18n::tr("settings.simple"), i18n::tr("settings.vault")}, state.mode == EditorMode::Vault ? 1 : 0, metrics.panelFontSize,
                        [&state](int index) {
                            state.mode = index == 1 ? EditorMode::Vault : EditorMode::Simple;
                            persistSettings(state);
                            app::requestUpdate();
                        });
            rowY += geometry.rowHeight;

                                }
                                if (state.settingsCategory == SettingsCategory::Editor) {
            rowLabel(ui, colors, metrics, uiFont, "settings.statusBar", i18n::tr("settings.status_bar"), i18n::tr("settings.status_bar_hint"),
                     0.0f, rowY, labelWidth, geometry);
            segmentedAt(ui, colors, "settings.statusBar",
                        controlX, rowY + plainRowControlY, controlWidth,
                        {i18n::tr("settings.show"), i18n::tr("settings.hide")}, state.showStatusBar ? 0 : 1, metrics.panelFontSize,
                        [&state](int index) {
                            state.showStatusBar = index == 0;
                            applyShowStatusBar(state);
                        });
            rowY += geometry.rowHeight;

                                }
                                if (state.settingsCategory == SettingsCategory::FileSystem) {
                                    fileAssocCard(ui, state, colors, metrics, uiFont, rowY, geometry.contentWidth, associationSpace);
                                    rowY += fileAssocCardHeight(metrics, geometry.contentWidth, associationSpace, state) + 12.0f;
                                }
                                if (state.settingsCategory == SettingsCategory::Editor) {
                                    rowLabel(ui, colors, metrics, uiFont, "settings.lineNumbers", i18n::tr("settings.line_numbers"), i18n::tr("settings.line_numbers_hint"),
                                             0.0f, rowY, labelWidth, geometry);
                                    segmentedAt(ui, colors, "settings.lineNumbers",
                                                controlX, rowY + plainRowControlY, controlWidth,
                                                {i18n::tr("settings.show"), i18n::tr("settings.hide")}, state.showLineNumbers ? 0 : 1,
                                                metrics.panelFontSize, [&state](int index) {
                                                    state.showLineNumbers = index == 0;
                                                    persistSettings(state);
                                                    app::requestUpdate();
                                                });
                                    rowY += geometry.rowHeight;
                                    rowLabel(ui, colors, metrics, uiFont, "settings.readableWidth", i18n::tr("settings.readable_width"), i18n::tr("settings.readable_width_hint"),
                                             0.0f, rowY, labelWidth, geometry);
                                    segmentedAt(ui, colors, "settings.readableWidth",
                                                controlX, rowY + plainRowControlY, controlWidth,
                                                {i18n::tr("settings.on"), i18n::tr("settings.off")}, state.readableWidth ? 0 : 1,
                                                metrics.panelFontSize, [&state](int index) {
                                                    state.readableWidth = index == 0;
                                                    persistSettings(state);
                                                    app::requestUpdate();
                                                });
                                    rowY += geometry.rowHeight;
                                }
                                rowLabel(ui, colors, metrics, uiFont, "settings.resetRow", i18n::tr("settings.reset"),
                                         i18n::tr("settings.reset_hint"),
                                         0, rowY, labelWidth, geometry);
                                flatButton(ui,colors,"settings.reset",i18n::tr("settings.reset_button"),
                                    controlX,rowY+hintRowControlY,controlWidth,kControlHeight,false,metrics.panelFontSize,[&state] {
                                        requestRisk(state,i18n::tr("settings.reset_title"),i18n::tr("settings.reset_confirm_hint"),i18n::tr("settings.reset"),[&state]{resetViewSettings(state);app::requestUpdate();});
                                    },true);
                                rowY+=geometry.rowHeight;
                            })
                            .build();
                    })
                    .build();
                if (maxOffset > 0.0f) components::scroll(ui, "settings.main.scrollbar")
                    .theme(colors.tokens)
                    .scrollStateId(scrollId)
                    .position(pageWidth - 12.0f, scrollTop)
                    .size(8.0f, scrollHeight).viewport(scrollHeight)
                    .content(contentHeight).offset(state.settingsScroll)
                    .step(scrollStep).build();
            })
            .build();
    }).build();
}

} // namespace neo
