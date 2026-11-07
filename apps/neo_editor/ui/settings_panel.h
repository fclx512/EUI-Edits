#pragma once

#include "model/font_catalog.h"
#include "model/theme_loader.h"
#include "model/settings.h"
#include "platform/file_assoc.h"
#include "platform/font_safety.h"
#include "eui/json.h"
#include "state/app_actions.h"
#include "ui/metrics.h"
#include "ui/settings_header.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/ui_language.h"
#include "ui/about_page.h"
#include "ui/theme_workbench.h"
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
    float viewportHeight = -1.0f;
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
                       std::function<void()> onClick, bool danger = false, bool disabled = false) {
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
        .radius(neo::corner(colors, 6.0f))
        .fontSize(fontSize)
        .text(label)
        .textColor(textColor)
        .colors(normal, hover, pressed)
        .shadow(0.0f, 0.0f, 0.0f, transparentColor())
        .border(0.0f, transparentColor())
        .preserveFocusOnPress()
        .disabled(disabled)
        .onClick(std::move(onClick))
        .transition(quickTransition())
        .build();
}

// ── 主题文件入口（T13 第一阶段）──────────────────────────────────────────
// 实现放在使用它的这一层（设置面板的行回调），而不是 state/app_actions.cpp：
// 本轮只允许改那个 .cpp 里**已有**的函数，新入口跟着行留在本文件。
// 三步顺序固定：校验并生效 → 同步状态与清屏色 → 落盘 + 整屏失效。
// 失败（文件不存在 / JSON 坏 / 版本不认识）只弹 toast 说明，**不动当前配色**。

inline std::size_t editableThemeColorCount(const std::map<std::string, eui::Color>& values) {
    std::size_t count = 0;
    for (const auto& field : themeworkbench::fields()) if (values.find(field.name) != values.end()) ++count;
    return count;
}

inline bool applyThemeFilePath(AppState& state, const std::string& path) {
    std::string error;
    if (!themeloader::load(path, error)) {
        showToast(state, i18n::tr("settings.theme_unavailable"), error);
        app::requestUpdate();
        return false;
    }
    state.themeFile = path;
    mutableAppConfig().clearColorValue = editorColors().window;
    if (!persistSettings(state)) {
        showToast(state, i18n::tr("settings.theme_session_only"), path);
    } else if (activeTheme().version == themeloader::kSchemaVersion) {
        showToast(state, i18n::tr("settings.theme_loaded"),
                  i18n::tr(activeTheme().baseLight ? "settings.theme_light_only" : "settings.theme_dark_only"));
    } else {
        const ThemeFileData& theme = activeTheme();
        const auto lightCount = editableThemeColorCount(theme.light.colors);
        const auto darkCount = editableThemeColorCount(theme.dark.colors);
        const std::string summary = i18n::format("settings.theme_counts",{
            {"name",theme.name},{"light",std::to_string(lightCount)},
            {"dark",std::to_string(darkCount)},
            {"unmapped",std::to_string(52-lightCount-darkCount)}});
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
    if (!persistSettings(state)) showToast(state, i18n::tr("settings.theme_session_only"), {});
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
    // 导入先审阅，再由工作台创建独立草稿；此处不触及当前生效主题。
    return theme_workbench_ui::reviewFile(state, result.paths.front());
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
        .x(x).y(y).size(width, height)
        .radius(neo::corner(colors, 6.0f))
        .states(normal, hover, colors.pressed)
        .cursor(eui::CursorShape::Hand)
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

inline int rowHintLines(const std::string& hint, const char* fontFamily, float size,
                        float width, float lineHeight) {
    if (hint.empty()) return 0;
    core::TextStyle style;
    style.text = hint; style.fontFamily = fontFamily;
    style.fontSize = size; style.lineHeight = lineHeight;
    style.wrap = true; style.maxWidth = std::max(1.0f, width);
    return std::max(1, static_cast<int>(std::round(core::TextPrimitive::measureTextSize(style).y / lineHeight)));
}

// Measure each row independently. A long font name must not enlarge unrelated
// rows, and the scroll extent must use the exact geometry used for drawing.
inline PanelGeometry settingsRowGeometry(const PanelGeometry& base,
                                         const UiMetrics& metrics,
                                         const char* fontFamily,
                                         const std::string& label,
                                         const std::string& hint) {
    PanelGeometry row = base;
    core::TextStyle text;
    text.fontFamily = fontFamily; text.wrap = true;
    text.maxWidth = std::max(1.0f, row.labelWidth);
    text.text = label; text.fontSize = metrics.panelLabelFontSize;
    text.fontWeight = 600; text.lineHeight = metrics.panelLabelFontSize + 7.0f;
    const auto labelInk = core::TextPrimitive::measureTextInk(text);
    const float labelHeight = labelInk.y - labelInk.x;
    text.text = hint; text.fontSize = metrics.panelHintFontSize;
    text.fontWeight = 400; text.lineHeight = metrics.panelHintFontSize + 5.0f;
    const auto hintInk = hint.empty() ? core::Vec2{} : core::TextPrimitive::measureTextInk(text);
    const float inkHeight = labelHeight + (hint.empty() ? 0.0f : 4.0f + hintInk.y - hintInk.x);
    const float textHeight = std::max(kControlHeight, std::ceil(inkHeight)) + 24.0f;
    row.cardHeight = textHeight + (row.stacked ? 10.0f + kControlHeight + 12.0f : 0.0f);
    row.controlTop = row.stacked ? textHeight + 10.0f : (row.cardHeight - kControlHeight) * .5f;
    row.rowHeight = row.cardHeight + 8.0f;
    return row;
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
    if (colors.tokens.metrics.corners == components::theme::CornerStyle::Square) {
        // The Square settings pane is one flat surface. Keep the existing row
        // geometry and place a hairline in the existing gap between rows.
        ui.rect(id + ".separator")
            .position(x, y + geometry.rowHeight - 1.0f)
            .size(geometry.contentWidth, 1.0f)
            .color(components::theme::withAlpha(colors.border, .45f))
            .hitTestMode(core::dsl::HitTestMode::None).build();
    } else {
        ui.rect(id + ".card")
            .position(x, y).size(geometry.contentWidth, geometry.cardHeight)
            .radius(neo::corner(colors, 8.0f)).color(colors.editor)
            .border(1.0f, components::theme::withAlpha(colors.border, .45f)).build();
    }
    // Centre the complete ink block, not nominal line boxes. Use the renderer's
    // actual wrapping and the same explicit line height in measurement/drawing.
    const float labelLineH = metrics.panelLabelFontSize + 7.0f;
    const float hintLineH = metrics.panelHintFontSize + 5.0f;
    const int hintLines = rowHintLines(hint, fontFamily, metrics.panelHintFontSize, width, hintLineH);
    const float textAreaH = geometry.cardHeight - (geometry.stacked ? 22.0f + kControlHeight : 0.0f);
    core::TextStyle labelStyle;
    labelStyle.text = text; labelStyle.fontFamily = fontFamily;
    labelStyle.fontSize = metrics.panelLabelFontSize; labelStyle.fontWeight = 600;
    labelStyle.lineHeight = labelLineH;
    labelStyle.wrap = true; labelStyle.maxWidth = std::max(1.0f, width);
    const auto labelInk = core::TextPrimitive::measureTextInk(labelStyle);
    core::TextStyle hintStyle;
    hintStyle.text = hint; hintStyle.fontFamily = fontFamily;
    hintStyle.fontSize = metrics.panelHintFontSize; hintStyle.lineHeight = hintLineH;
    hintStyle.wrap = true; hintStyle.maxWidth = std::max(1.0f, width);
    const auto hintInk = hint.empty() ? core::Vec2{} : core::TextPrimitive::measureTextInk(hintStyle);
    const float labelInkHeight = labelInk.y - labelInk.x;
    const float contentH = labelInkHeight + (hint.empty() ? 0.0f :
        4.0f + hintInk.y - hintInk.x);
    const float textTop = y + std::max(6.0f, (textAreaH - contentH) * 0.5f);
    ui.text(id + ".label")
        .x(x + 18.0f)
        .y(textTop - labelInk.x)
        .size(width, core::TextPrimitive::measureTextSize(labelStyle).y)
        .text(text).wrap(true).maxWidth(std::max(1.0f, width))
        .fontFamily(fontFamily)
        .fontSize(metrics.panelLabelFontSize).fontWeight(600)
        .lineHeight(labelLineH).verticalAlign(eui::VerticalAlign::Top)
        .color(colors.text)
        .build();
    if (!hint.empty()) {
        ui.text(id + ".hint")
            .x(x + 18.0f)
            .y(textTop + labelInkHeight + 4.0f - hintInk.x)
            .size(width, static_cast<float>(hintLines) * hintLineH)
            .text(hint).wrap(true)
            .fontFamily(fontFamily)
            .fontSize(metrics.panelHintFontSize)
            .lineHeight(hintLineH).maxWidth(std::max(1.0f, width))
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
struct AssociationGeometry {
    float scale, rowHeight, listTop, viewport, footerHeight, height;
};
inline AssociationGeometry associationGeometry(const UiMetrics& metrics, float width,
                                                float availableHeight, const AppState& state) {
    AssociationGeometry g{};
    g.scale = std::max(1.0f, metrics.panelFontSize / 14.0f);
    g.rowHeight = (width - 36 < 500 ? 62.0f : 54.0f) * g.scale;
    g.listTop = 226.0f * g.scale;
    g.footerHeight = 188.0f * g.scale;
    const float fit = std::floor(std::max(0.0f, availableHeight-g.listTop-g.footerHeight)/g.rowHeight);
    const float rows = static_cast<float>(associationRows(state).size());
    g.viewport = std::max(4.0f, std::min(rows, std::clamp(fit, 4.0f, 9.0f))) * g.rowHeight;
    g.height = g.listTop + g.viewport + g.footerHeight;
    return g;
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
inline const char* associationDefaultLabel(fileassoc::DefaultApp value) {
    using fileassoc::DefaultApp;
    switch (value) {
        case DefaultApp::EUIEdits: return "EUI-Edits";
        case DefaultApp::Other: return i18n::tr("assoc.other");
        case DefaultApp::None: return i18n::tr("assoc.none");
        case DefaultApp::Unknown: return i18n::tr("assoc.unknown");
    }
    return i18n::tr("assoc.unknown");
}

inline void reportAssociationResult(AppState& state, const fileassoc::RegisterOutcome& result) {
    showToast(state, i18n::tr(result.ok ? "settings.associations_updated" : "settings.associations_failed"),
              result.ok ? i18n::tr("settings.associations_updated_hint") : result.error);
    refreshFileAssocState(state);
    if (result.ok && state.associationCustomOpen)
        state.associationSelected = fileassoc::registeredExtensions();
    app::requestUpdate();
}

inline void openAssociationDefaults(AppState& state) {
    // Arm before launching Settings. A native activation message wakes compose,
    // including when the editor was idle for the entire trip to Settings.
    platform::chrome::nextActivation = [&state] {
        state.associationRefreshPending = true;
        app::requestUpdate();
    };
    if (!fileassoc::openDefaultAppsSettings()) {
        platform::chrome::nextActivation = {};
        showToast(state, i18n::tr("assoc.settings_failed"), i18n::tr("assoc.settings_failed_hint"));
        app::requestUpdate();
    }
}

inline void requestAssociationSelection(AppState& state, std::vector<std::string> selection) {
    refreshFileAssocState(state);
    bool removing = false;
    std::string blocked;
    const bool clearingExisting = selection.empty() && state.fileAssocEntriesPresent;
    for (const auto& type : state.associationStatuses) {
        if (std::find(selection.begin(), selection.end(), type.extension) != selection.end()) continue;
        removing = removing || type.registered;
        if (fileassoc::removalBlocked(type, clearingExisting)) {
            if (!blocked.empty()) blocked += ", ";
            blocked += "." + type.extension;
        }
    }
    if (!blocked.empty()) {
        showToast(state, i18n::tr("assoc.removal_blocked"), blocked);
        app::requestUpdate();
        return;
    }
    const bool clearing = selection.empty();
    auto apply = [&state, selection = std::move(selection)] {
        reportAssociationResult(state, fileassoc::applySelection(selection));
    };
    if (removing || clearing) {
        requestRisk(state, i18n::tr(clearing ? "assoc.clear_title" : "assoc.remove_some_title"),
                    i18n::tr(clearing ? "assoc.clear_hint" : "assoc.remove_some_hint"),
                    i18n::tr(clearing ? "assoc.clear_confirm" : "assoc.apply"), std::move(apply));
    } else {
        apply();
    }
}

inline const char* associationOverviewHint(fileassoc::QuickAction action) {
    using fileassoc::QuickAction;
    switch (action) {
        case QuickAction::Add: return i18n::tr("assoc.quick_add_hint");
        case QuickAction::Repair: return i18n::tr("assoc.quick_repair_hint");
        case QuickAction::ManageDefaults: return i18n::tr("assoc.quick_manage_hint");
        default: return i18n::tr("assoc.quick_defaults_hint");
    }
}

inline float associationOverviewHintHeight(const AppState& state, const UiMetrics& metrics, float width) {
    const auto action = fileassoc::quickAction(state.fileAssocRegistered, state.associationStatuses);
    const float lineHeight = metrics.panelHintFontSize + 5.0f;
    return rowHintLines(associationOverviewHint(action), uiFontFamily(state), metrics.panelHintFontSize,
                        std::max(1.0f, width - 36.0f), lineHeight) * lineHeight + 4.0f;
}

inline float fileAssocCardHeight(const UiMetrics& metrics, float width, float availableHeight, const AppState& state) {
    if (state.associationCustomOpen) return associationGeometry(metrics, width, availableHeight, state).height;
    const float scale = std::max(1.0f, metrics.panelFontSize / 14.0f);
    return 212.0f * scale + associationOverviewHintHeight(state, metrics, width);
}

inline void fileAssocOverview(eui::Ui& ui, AppState& state, const EditorColors& colors,
                             const UiMetrics& metrics, const char* font, float y, float width) {
    using fileassoc::QuickAction;
    const auto action = fileassoc::quickAction(state.fileAssocRegistered, state.associationStatuses);
    const char* primary = action == QuickAction::Add ? i18n::tr("assoc.quick_add")
        : action == QuickAction::Repair ? i18n::tr("assoc.quick_repair")
        : action == QuickAction::ManageDefaults ? i18n::tr("assoc.quick_manage") : i18n::tr("assoc.quick_default");
    const float inner = std::max(0.0f, width - 36.0f);
    const float scale = std::max(1.0f, metrics.panelFontSize / 14.0f);
    const float hintH = associationOverviewHintHeight(state, metrics, width);
    const auto widthFor = [&](const char* label) {
        return std::min(inner, core::TextPrimitive::measureTextWidth(label, font, metrics.panelFontSize) + 28.0f);
    };

    ui.rect("settings.fileAssoc.card").position(0, y).size(width, 212.0f * scale + hintH)
        .radius(neo::corner(colors, 8.0f)).color(colors.editor).border(1, colors.border).build();
    ui.text("settings.fileAssoc.label").position(18, y + 12 * scale).size(inner, 28 * scale)
        .text(i18n::tr("assoc.title")).fontFamily(font).fontSize(metrics.panelLabelFontSize).fontWeight(600)
        .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
    ui.text("settings.fileAssoc.description").position(18, y + 44 * scale).size(inner, hintH)
        .text(associationOverviewHint(action)).wrap(true).fontFamily(font).fontSize(metrics.panelHintFontSize)
        .lineHeight(metrics.panelHintFontSize + 5.0f).color(colors.textMuted).build();

    float statusY = y + 52 * scale + hintH;
    for (const char* extension : {"txt", "md"}) {
        const std::string ext = extension;
        const std::string id = "settings.fileAssoc.quick." + ext;
        const auto it = std::find_if(state.associationStatuses.begin(), state.associationStatuses.end(),
                                     [&](const auto& type) { return type.extension == ext; });
        const bool registered = it != state.associationStatuses.end() && it->registered;
        const auto actualDefault = it == state.associationStatuses.end() ? fileassoc::DefaultApp::Unknown : it->defaultApp;
        const auto status = i18n::format("assoc.state", {
            {"added", i18n::tr(registered ? "assoc.added" : "assoc.not_added")},
            {"default", associationDefaultLabel(actualDefault)}});
        fileFormatPill(ui, id, "." + ext, 18, statusY + (34 * scale - kFileFormatPillHeight) * .5f,
                       58, colors, ext == "txt" ? colors.iconTxt : colors.iconMd, font, metrics.panelHintFontSize);
        ui.text(id + ".state").position(86, statusY).size(std::max(0.0f, inner - 68), 34 * scale)
            .text(status).wrap(true).fontFamily(font).fontSize(metrics.panelHintFontSize)
            .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
        statusY += 34 * scale;
    }

    flatButton(ui, colors, "settings.fileAssoc.quick.action", primary, 18, statusY + 5 * scale,
               widthFor(primary), 34 * scale, true, metrics.panelFontSize, [&state, action] {
        if (action == QuickAction::Add) {
            reportAssociationResult(state, fileassoc::registerAsDefault());
        } else if (action == QuickAction::Repair) {
            const auto registered = fileassoc::registeredExtensions();
            if (!registered.empty()) reportAssociationResult(state, fileassoc::applySelection(registered));
            else { refreshFileAssocState(state); app::requestUpdate(); }
        } else {
            openAssociationDefaults(state);
        }
    });
    const char* custom = i18n::tr("assoc.custom");
    flatButton(ui, colors, "settings.fileAssoc.custom", custom, 18, statusY + 47 * scale, widthFor(custom),
               30 * scale, false, metrics.panelFontSize, [&state] {
        state.associationSelected = fileassoc::registeredExtensions();
        state.associationCustomOpen = true;
        state.associationSearch.clear();
        state.associationCategory = 0;
        state.settingsScroll = 0.0f;
        app::requestUpdate();
    });
}

inline void fileAssocCard(eui::Ui& ui,AppState& state,const EditorColors& colors,const UiMetrics& metrics,const char* uiFont,float y,float width,float availableHeight) {
    if (!state.associationCustomOpen) {
        fileAssocOverview(ui, state, colors, metrics, uiFont, y, width);
        return;
    }
    float& offset=ui.state<float>("settings.fileAssoc.offset");
    const float inner=std::max(0.0f,width-36);
    const auto layout=associationGeometry(metrics,width,availableHeight,state);
    const float scale=layout.scale, rowH=layout.rowHeight, viewport=layout.viewport;
    const float listY=y+layout.listTop;
    const auto rows=associationRows(state);
    const auto label=[&](const char* id, const std::string& text, float top, float h, bool title=false) {
        ui.text(id).position(18,top).size(inner,h).text(text).wrap(true).fontFamily(uiFont)
            .fontSize(title?metrics.panelLabelFontSize:metrics.panelHintFontSize)
            .fontWeight(title?600:400).color(title?colors.text:colors.textMuted)
            .verticalAlign(eui::VerticalAlign::Center).build();
    };
    const auto buttonWidth=[&](const char* text) {
        return std::min(inner,core::TextPrimitive::measureTextWidth(text,uiFont,metrics.panelFontSize)+28.0f);
    };
    ui.rect("settings.fileAssoc.card").position(0,y).size(width,layout.height).radius(neo::corner(colors, 8.0f)).color(colors.editor).border(1,colors.border).build();
    const char* back = i18n::tr("assoc.back");
    flatButton(ui, colors, "settings.fileAssoc.back", back, 18, y + 12 * scale, buttonWidth(back),
               32 * scale, false, metrics.panelFontSize, [&state] {
        state.associationCustomOpen = false;
        state.settingsScroll = 0.0f;
        app::requestUpdate();
    });
    label("settings.fileAssoc.description", i18n::tr("settings.assoc_description"), y + 48 * scale, 44 * scale);
    ui.text("settings.fileAssoc.selected").position(18,y+96*scale).size(inner,32*scale)
        .text(i18n::format("assoc.selected_count",{{"count",std::to_string(state.associationSelected.size())}}))
        .fontFamily(uiFont).fontSize(metrics.panelHintFontSize).color(colors.textMuted).verticalAlign(eui::VerticalAlign::Center).build();
    segmentedAt(ui,colors,"settings.fileAssoc.category",18,y+140*scale,inner,
        {i18n::tr("assoc.all"),i18n::tr("assoc.text"),i18n::tr("assoc.data"),i18n::tr("assoc.code")},state.associationCategory,metrics.panelFontSize,[&state,&offset](int index){state.associationCategory=index;offset=0;app::requestUpdate();});
    components::input(ui,"settings.fileAssoc.search").position(18,y+182*scale).size(inner,32*scale).value(state.associationSearch).placeholder(i18n::tr("assoc.search"))
        .fontFamily(uiFont).fontSize(metrics.panelFontSize).inset(8).theme(colors.tokens).onChange([&state,&offset](const std::string& value){state.associationSearch=value;offset=0;app::requestUpdate();}).build();

    std::size_t adding=0,removing=0;
    for(const auto& status:state.associationStatuses) {
        const bool selected=std::find(state.associationSelected.begin(),state.associationSelected.end(),status.extension)!=state.associationSelected.end();
        if(selected && !status.registered) ++adding;
        if(!selected && (status.registered || status.defaultApp==fileassoc::DefaultApp::EUIEdits)) ++removing;
    }
    const bool repair=state.fileAssocEntriesPresent&&!state.fileAssocRegistered;
    const float footer=listY+viewport;
    label("settings.fileAssoc.changes",
        i18n::format("assoc.changes",{{"add",std::to_string(adding)},{"remove",std::to_string(removing)}}) +
            (repair ? std::string(" · ")+i18n::tr("assoc.repair_hint") : std::string{}),footer+8*scale,34*scale);
    flatButton(ui, colors, "settings.fileAssoc.register", i18n::tr("assoc.apply"), 18, footer + 48 * scale,
               buttonWidth(i18n::tr("assoc.apply")), 34 * scale, true, metrics.panelFontSize,
               [&state] { requestAssociationSelection(state, state.associationSelected); },
               false, !adding && !removing && !repair);
    flatButton(ui, colors, "settings.fileAssoc.defaults.open", i18n::tr("assoc.defaults"), 18, footer + 94 * scale,
               buttonWidth(i18n::tr("assoc.defaults")), 34 * scale, false, metrics.panelFontSize,
               [&state] { openAssociationDefaults(state); });
    flatButton(ui, colors, "settings.fileAssoc.remove", i18n::tr("assoc.clear"), 18, footer + 138 * scale,
               buttonWidth(i18n::tr("assoc.clear")), 34 * scale, false, metrics.panelFontSize,
               [&state] { requestAssociationSelection(state, {}); }, true, !state.fileAssocEntriesPresent);
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
            const char* defaultLabel = associationDefaultLabel(status.defaultApp);
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
                rowUi.rect(id+".hit").fill().radius(neo::corner(colors, 6.0f)).states(transparentColor(),colors.rowHover,colors.pressed)
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
                rowUi.rect(id+".checkbox").position(10,center-box*.5f).size(box,box).radius(neo::corner(colors, 4.0f))
                    .color(selected?colors.accent:colors.editor).border(selected?0.0f:1.5f,colors.textMuted).build();
                if(selected) components::vector_icon::drawCheckmark(rowUi,id+".check",12,center-7,14,rgba(1,1,1,1));
                fileFormatPill(rowUi, id, "." + ext, 42, center - kFileFormatPillHeight * .5f,
                               pillWidth, colors, tint, uiFont, metrics.panelHintFontSize);
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
                          const char* fontFamily,
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
    const float leftWidth = std::min(std::max(0.0f, geometry.contentWidth-kButtonWidth-12),
        std::max(kButtonWidth, core::TextPrimitive::measureTextWidth(leftLabel,fontFamily,metrics.panelFontSize)+24));
    flatButton(ui, colors, leftId, leftLabel, kPadding, geometry.buttonY,
               leftWidth, kButtonHeight, false, metrics.panelFontSize, std::move(onLeft));
    flatButton(ui, colors, "settings.footer.primary", rightLabel,
               kPadding + geometry.contentWidth - kButtonWidth, geometry.buttonY,
               kButtonWidth, kButtonHeight, true, metrics.panelFontSize, std::move(onRight));
}

// 字体选择页。行高固定（virtualList 的前提），列表里只画字体名、
// 不按字体自身渲染每一行——那会把几十个字体文件全塞进字体栈与字形图集，
// 图集填满后是静默不画（见 反馈调研 §2.7）。预览只给"当前选中"这一个字体做一份。
inline void openFontPicker(AppState& state, FontPickerTarget target) {
    state.fontPicker = target;
    state.fontSearch.clear();
    state.fontSearchFocusPending = true;
    state.fontListScroll = 0;
    app::requestUpdate();
}

struct FontPickerSearchLayout {
    float searchY = 88.0f, searchHeight = 34.0f, countY = 0, countHeight = 0, sampleY = 0;
    float rowHeight = 36.0f, minimumHeight = 0;
};
inline FontPickerSearchLayout fontPickerSearchLayout(const UiMetrics& metrics) {
    FontPickerSearchLayout layout;
    layout.searchHeight = std::max(34.0f, metrics.panelFontSize + 18.0f);
    layout.countY = layout.searchY + layout.searchHeight + 8.0f;
    layout.countHeight = metrics.panelHintFontSize + 8.0f;
    layout.sampleY = layout.countY + layout.countHeight + 10.0f;
    layout.rowHeight = std::max(36.0f, metrics.panelFontSize + 22.0f);
    layout.minimumHeight = layout.sampleY + layout.rowHeight * 3.0f + 66.0f;
    return layout;
}

inline void fontPickerContent(eui::Ui& ui,
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
                    state.fontListScroll = 0;
                    app::requestUpdate();
                });

    const std::string currentName = currentFile.empty() ? std::string(i18n::tr("settings.font_preset")) : fonts::displayNameFor(currentFile);
    const std::string currentNotes =
        std::string(currentFile.empty() ? (target == FontPickerTarget::Code ? i18n::tr("settings.font_default_mono_note") : i18n::tr("settings.font_default_body_note"))
            : target == FontPickerTarget::Code ? i18n::tr("settings.font_mono_note") : "") +
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

    const auto layout = fontPickerSearchLayout(metrics);
    const float clearWidth = std::max(56.0f, core::TextPrimitive::measureTextWidth(
        i18n::tr("settings.font_search_clear"), uiFontFamily(state), metrics.panelFontSize) + 20.0f);
    components::input(ui, "settings.fontPicker.search")
        .position(kPadding, layout.searchY)
        .size(std::max(1.0f, geometry.contentWidth - clearWidth - 10.0f), layout.searchHeight)
        .value(state.fontSearch).placeholder(i18n::tr("settings.font_search"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize).inset(8).theme(colors.tokens)
        .onChange([&state](const std::string& value) {
            state.fontSearch = value;
            state.fontListScroll = 0;
            app::requestUpdate();
        }).build();
    flatButton(ui, colors, "settings.fontPicker.clear", i18n::tr("settings.font_search_clear"),
        kPadding + geometry.contentWidth - clearWidth, layout.searchY, clearWidth, layout.searchHeight,
        false, metrics.panelFontSize, [&state] {
            state.fontSearch.clear();
            state.fontListScroll = 0;
            state.fontSearchFocusPending = true;
            app::requestUpdate();
        }, false, state.fontSearch.empty());
    if (state.fontSearchFocusPending) {
        ui.requestFocus("settings.fontPicker.search.hit");
        state.fontSearchFocusPending = false;
    }
    // 预览只使用当前字体，不让每个列表行都加载各自的字体文件。
    const float sampleY = layout.sampleY;
    const float sampleFontSize = metrics.panelFontSize + 3.0f;
    const float sampleLineHeight = std::round(sampleFontSize * 1.4f);
    const bool codePreview = target == FontPickerTarget::Code;
    const float requestedSampleHeight = 40.0f + sampleLineHeight * (codePreview ? 3 : 1) + 12;
    const float previewSpace = std::max(0.0f, geometry.footerRuleY - sampleY - layout.rowHeight * 2.0f - 12.0f);
    const bool showSample = requestedSampleHeight <= previewSpace;
    const float sampleHeight = showSample ? requestedSampleHeight : 0;
    const std::string sampleText = codePreview
        ? "const item  = 42;\nconst total = item * 2;\nreturn total;"
        : "永和九年 岁在癸丑 Hello 123";
    if (showSample) {
        ui.rect("settings.fontPicker.preview.card").position(kPadding, sampleY)
            .size(geometry.contentWidth, sampleHeight).radius(neo::corner(colors, 8.0f))
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
    const float rowHeight = layout.rowHeight;
    ui.rect("settings.fontPicker.list.card").position(kPadding, listY)
        .size(geometry.contentWidth, listHeight).radius(neo::corner(colors, 8.0f))
        .color(colors.editor).border(1, colors.border).build();

    std::vector<const fonts::Entry*> visibleEntries;
    fonts::Entry selectedOutsideCatalog;
    bool selectedPathInCatalog = false;
    for (const fonts::Entry& entry : entries) {
        const bool selected = !currentFile.empty() && entry.path == currentFile;
        selectedPathInCatalog = selectedPathInCatalog || selected;
        if ((target != FontPickerTarget::Code || entry.monospaceHint || selected) &&
            fonts::matchesSearch(entry, state.fontSearch)) {
            visibleEntries.push_back(&entry);
        }
    }
    if (target == FontPickerTarget::Code && !currentFile.empty() && !selectedPathInCatalog) {
        selectedOutsideCatalog.displayName = fonts::displayNameFor(currentFile);
        selectedOutsideCatalog.path = currentFile;
        if (fonts::matchesSearch(selectedOutsideCatalog, state.fontSearch)) visibleEntries.push_back(&selectedOutsideCatalog);
    }

    ui.text("settings.fontPicker.count").position(kPadding, layout.countY)
        .size(geometry.contentWidth, layout.countHeight)
        .text(i18n::format("settings.font_search_count", {{"count", std::to_string(visibleEntries.size())}}))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize).color(colors.textMuted).build();

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
            .text(!state.fontSearch.empty() ? i18n::tr("settings.font_search_empty") : target == FontPickerTarget::Code
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
                            .radius(neo::corner(colors, 4.0f))
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
                                .radius(neo::corner(colors, 4.0f)).color(colors.toolbar).border(1, colors.border).build();
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

    footerButtons(ui, colors, metrics, uiFontFamily(state), geometry,
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

inline void fontPickerPage(eui::Ui& ui, AppState& state, const EditorColors& colors,
                           const UiMetrics& metrics, const PanelGeometry& availableGeometry) {
    const float height = std::max(0.0f, availableGeometry.viewportHeight);
    const float width = availableGeometry.contentWidth + kPadding * 2.0f;
    const float contentHeight = std::max(height, fontPickerSearchLayout(metrics).minimumHeight);
    const float maxOffset = std::max(0.0f, contentHeight - height);
    PanelGeometry geometry = availableGeometry;
    geometry.footerRuleY = contentHeight - 86.0f;
    float& offset = ui.state<float>("settings.fontPicker.pageOffset");
    if (state.fontSearchFocusPending) offset = 0;
    offset = std::clamp(offset, 0.0f, maxOffset);
    ui.stack("settings.fontPicker.viewport").size(width, height).clip()
        .scrollState("settings.fontPicker.viewport", offset, maxOffset, 36.0f)
        .onScrollOffsetChanged([&offset](float value) { offset = value; }).content([&] {
            ui.stack("settings.fontPicker.content").size(width, contentHeight)
                .scrollContentFrom("settings.fontPicker.viewport").content([&] {
                    fontPickerContent(ui, state, colors, metrics, geometry);
                }).build();
        }).build();
    if (maxOffset > 0) components::scroll(ui, "settings.fontPicker.pageScrollbar")
        .theme(colors.tokens).scrollStateId("settings.fontPicker.viewport")
        .position(width - 12, 0).size(8, height).viewport(height).content(contentHeight)
        .offset(offset).step(36).build();
}

struct ThemeListLayout {
    float hintHeight = 0.0f, listY = 0.0f, contentHeight = 0.0f;
};

inline ThemeListLayout themeListLayout(const UiMetrics& metrics, const char* font,
                                        float width, float availableHeight) {
    core::TextStyle hint;
    hint.text = i18n::tr("settings.theme_library_hint");
    hint.fontFamily = font;
    hint.fontSize = metrics.panelHintFontSize;
    hint.lineHeight = metrics.panelHintFontSize + 5.0f;
    hint.wrap = true;
    hint.maxWidth = std::max(1.0f, width);
    ThemeListLayout layout;
    layout.hintHeight = std::max(hint.lineHeight, core::TextPrimitive::measureTextSize(hint).y);
    layout.listY = 64.0f + layout.hintHeight + 12.0f;
    // Keep a usable list and every action in the scrollable page even when a
    // wrapped header / navigation leaves very little vertical room.
    layout.contentHeight = std::max(availableHeight, layout.listY + 64.0f + 55.0f + 86.0f);
    return layout;
}

inline int settingsNavigationColumns(const AppState& state, float width) {
    const auto metrics = uiMetrics(state);
    float needed = 88.0f;
    for (const char* id : {"settings.appearance", "settings.editor", "settings.files_system", "settings.about_nav"})
        needed = std::max(needed, core::TextPrimitive::measureTextWidth(
            i18n::tr(id), uiFontFamily(state), metrics.panelFontSize) + 24.0f);
    const float available = std::max(0.0f, width - 24.0f);
    return available / 4.0f - 4.0f >= needed ? 4 : available / 2.0f - 4.0f >= needed ? 2 : 1;
}

inline void themeListPage(eui::Ui& ui, AppState& state, const EditorColors& colors,
                          const UiMetrics& metrics, const PanelGeometry& availableGeometry) {
    if (theme_workbench_ui::session().catalogRefreshNeeded) {
        refreshThemeCatalog();
        theme_workbench_ui::session().catalogRefreshNeeded = false;
    }
    const float pageWidth = availableGeometry.contentWidth + kPadding * 2.0f;
    const float pageHeight = availableGeometry.viewportHeight >= 0.0f
        ? availableGeometry.viewportHeight : availableGeometry.footerRuleY + 86.0f;
    const auto layout = themeListLayout(metrics, uiFontFamily(state), availableGeometry.contentWidth, pageHeight);
    PanelGeometry geometry = availableGeometry;
    geometry.footerRuleY = layout.contentHeight - 86.0f;
    geometry.buttonY = geometry.footerRuleY + 8.0f;
    const float maxOffset = std::max(0.0f, layout.contentHeight - pageHeight);
    float& pageOffset = ui.state<float>("settings.themeList.pageOffset");
    pageOffset = std::clamp(pageOffset, 0.0f, maxOffset);
    ui.stack("settings.themeList.viewport").size(pageWidth, pageHeight).clip()
        .scrollState("settings.themeList.viewport", pageOffset, maxOffset, 36.0f)
        .onScrollOffsetChanged([&pageOffset](float value) { pageOffset = value; })
        .content([&] {
        ui.stack("settings.themeList.content").size(pageWidth, layout.contentHeight)
            .scrollContentFrom("settings.themeList.viewport").content([&] {
    ui.text("settings.themeList.title")
        .x(kPadding).y(18.0f).size(geometry.contentWidth, 32.0f)
        .text(i18n::tr("settings.theme_library"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize + 3.0f)
        .color(colors.text).build();
    ui.rect("settings.themeList.rule")
        .x(kPadding).y(54.0f).size(geometry.contentWidth, 1.0f)
        .color(colors.border).build();
    ui.text("settings.themeList.hint")
        .x(kPadding).y(64.0f).size(geometry.contentWidth, layout.hintHeight)
        .text(i18n::tr("settings.theme_library_hint"))
        .fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize)
        .lineHeight(metrics.panelHintFontSize + 5.0f).wrap(true)
        .verticalAlign(eui::VerticalAlign::Top)
        .color(colors.textMuted).build();

    const auto& entries = themeCatalog();
    const float listY = layout.listY;
    const float listHeight = geometry.footerRuleY - 55.0f - listY;
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
                        .fill().radius(neo::corner(colors, 4.0f))
                        .states(active ? colors.rowActive : transparentColor(),
                                active ? colors.rowActive : colors.rowHover, colors.pressed)
                        .transition(quickTransition()).cursor(eui::CursorShape::Hand)
                        .preserveFocusOnPress()
                        .onClick([&state, builtinRow, entry] {
                            if (builtinRow) {
                                // 内置配色与列表项一样点一下就生效；主题文件还在库里，随时可以点回来。
                                resetThemeFile(state);
                                state.themeListOpen = false;
                            } else if (const auto path = textfile::pathFromUtf8(entry.path);
                                       path.extension() == ".css" || path.filename() == "manifest.json") {
                                theme_workbench_ui::reviewFile(state, entry.path);
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
    flatButton(ui, colors, "settings.themeList.workbench", i18n::tr("settings.theme_workbench"),
               kPadding, geometry.footerRuleY - 40.0f,
               std::min(geometry.contentWidth, std::max(168.0f, core::TextPrimitive::measureTextWidth(
                   i18n::tr("settings.theme_workbench"), uiFontFamily(state), metrics.panelFontSize) + 24.0f)), 31.0f,
               true, metrics.panelFontSize, [&state] { openThemeWorkbench(state); });
    footerButtons(ui, colors, metrics, uiFontFamily(state), geometry,
                  "settings.themeList.file", i18n::tr("settings.pick_file"),
                  [&state] { chooseThemeFile(state); },
                  i18n::tr("settings.back"), [&state] { state.themeListOpen = false; app::requestUpdate(); });
            }).build();
        }).build();
}


// The same sample and production Markdown metrics drive measurement and render.
// It is read-only and does not create or modify a document or its undo history.
inline components::MarkdownStyle settingsPreviewStyle(const AppState& state) {
    return markdownStyle(state.editorFontSize, editorFontFamily(state), codeFontFamily(state),
                         editorColors(), state.theme);
}

inline float settingsSizePreview(eui::Ui* ui, const AppState& state, bool editor,
                                 const PanelGeometry& geometry, float y) {
    const auto metrics = uiMetrics(state);
    const auto& colors = editorColors();
    const std::string id = editor ? "settings.preview.editor" : "settings.preview.ui";
    const float innerWidth = std::max(1.0f, geometry.contentWidth - 36.0f);
    const float headerHeight = metrics.panelHintFontSize + 8.0f;
    const auto style = settingsPreviewStyle(state);
    const std::string sample = i18n::tr("settings.preview_document");
    const float sampleHeight = editor
        ? components::MarkdownBuilder::estimateHeight(sample, innerWidth, style) + 4.0f
        : metrics.menuBarHeight + metrics.menuItemHeight + 14.0f;
    const float height = 12.0f + headerHeight + 10.0f + sampleHeight + 16.0f;
    if (!ui) return height + 8.0f;
    ui->stack(id).position(0, y).size(geometry.contentWidth, height).clip()
        .hitTestMode(core::dsl::HitTestMode::None).content([&] {
            ui->rect(id + ".bg").fill().radius(neo::corner(colors, 8.0f))
                .color(colors.editor).border(1, colors.border).build();
            const std::string title = i18n::format(editor ? "settings.preview_body_size" : "settings.preview_ui_size",
                {{"size", std::to_string(static_cast<int>(std::lround(editor ? state.editorFontSize : state.uiFontSize)))}});
            ui->text(id + ".label").position(18, 12).size(innerWidth, headerHeight)
                .text(title).fontFamily(uiFontFamily(state)).fontSize(metrics.panelHintFontSize)
                .color(colors.textMuted).build();
            const float sampleY = 12.0f + headerHeight + 10.0f;
            if (editor) {
                components::markdown(*ui, id + ".document").position(18, sampleY)
                    .width(innerWidth).wrapContentHeight().markdown(sample).style(style).build();
            } else {
                ui->rect(id + ".toolbar").position(18, sampleY).size(innerWidth, metrics.menuBarHeight)
                    .radius(neo::corner(colors, 4.0f)).color(colors.toolbar).build();
                ui->text(id + ".menu").position(28, sampleY)
                    .size(std::max(1.0f, innerWidth - 20), metrics.menuBarHeight)
                    .text(i18n::tr("settings.preview_menu")).fontFamily(uiFontFamily(state))
                    .fontSize(metrics.menuFontSize).color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
                ui->text(id + ".sidebar").position(18, sampleY + metrics.menuBarHeight + 10)
                    .size(innerWidth, metrics.menuItemHeight).text(i18n::tr("settings.preview_sidebar"))
                    .fontFamily(uiFontFamily(state)).fontSize(metrics.vaultRowFontSize)
                    .color(colors.text).verticalAlign(eui::VerticalAlign::Center).build();
            }
        }).build();
    return height + 8.0f;
}

} // namespace settings_detail

inline void settingsPanelOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    using namespace settings_detail;
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    const char* uiFont = uiFontFamily(state);

    const bool newlyOpened = state.settingsOpen && !state.settingsOpenLast;
    if (newlyOpened) {
        popup_presentation::activate(state, popup_presentation::Owner::None);
        state.fontPicker = FontPickerTarget::None;
        state.themeListOpen = false;
        state.associationCustomOpen = false;
        if (state.settingsCategory == SettingsCategory::FileSystem) refreshFileAssocState(state);
        app::requestUpdate();
    }
    state.settingsOpenLast = state.settingsOpen;
    if (!state.settingsOpen) return;

    const float headerHeight = settingsHeaderLayout(state, screen.width).height;
    const bool compactNav = screen.width < 840.0f;
    const int navColumns = compactNav ? settingsNavigationColumns(state, screen.width) : 1;
    const float navHeight = compactNav ? 6.0f + 48.0f * ((4 + navColumns - 1) / navColumns) : 0.0f;
    const float navWidth = compactNav ? 0.0f : 176.0f;
    const float pageTop = headerHeight + navHeight;
    const float remainingWidth = std::max(0.0f, screen.width - navWidth);
    const float pageWidth = std::min(952.0f, remainingWidth);
    const float pageX = navWidth + (remainingWidth - pageWidth) * 0.5f;
    const float pageHeight = std::max(0.0f, screen.height - pageTop);

    const PanelGeometry geometry = [&] {
        PanelGeometry g;
        g.viewportHeight = pageHeight;
        g.contentWidth = std::max(0.0f, pageWidth - kPadding * 2.0f);
        g.controlWidth = std::min(std::max(0.0f, g.contentWidth - 36.0f),
                                  std::max(224.0f, metrics.panelFontSize * 16.0f));
        g.stacked = g.contentWidth < g.controlWidth + metrics.panelFontSize * 21.0f + 60.0f;
        if (g.stacked) g.controlWidth = std::max(0.0f, g.contentWidth - 36.0f);
        g.controlX = g.stacked ? 18.0f : g.contentWidth - g.controlWidth - 18.0f;
        g.labelWidth = std::max(0.0f, g.stacked ? g.contentWidth - 36.0f : g.controlX - 42.0f);
        g.footerRuleY = std::max(0.0f, pageHeight - 86.0f);
        g.buttonY = g.footerRuleY + 8.0f;
        return g;
    }();
    const float controlWidth = geometry.controlWidth;
    const float controlX = geometry.controlX;
    const float labelWidth = geometry.labelWidth;

    ui.stack("settings.page").size(screen.width, screen.height).zIndex(800).content([&] {
        ui.rect("settings.page.bg").fill().color(colors.window).build();
        settingsHeaderView(ui, state, screen);
        if (!compactNav) {
            ui.rect("settings.page.navBg")
                .position(0.0f, headerHeight).size(navWidth, pageHeight)
                .color(colors.panel).build();
        }

        const std::vector<std::pair<SettingsCategory, const char*>> categories = {
            {SettingsCategory::Appearance, i18n::tr("settings.appearance")},
            {SettingsCategory::Editor, i18n::tr("settings.editor")},
            {SettingsCategory::FileSystem, i18n::tr("settings.files_system")},
            {SettingsCategory::About, i18n::tr("settings.about_nav")},
        };
        const float navSlots = static_cast<float>(navColumns);
        float navY = headerHeight + (compactNav ? 7.0f : 20.0f);
        std::size_t categoryIndex = 0;
        for (const auto& entry : categories) {
            const bool active = state.settingsCategory == entry.first;
            flatButton(ui, colors, std::string("settings.nav.") + std::to_string(static_cast<int>(entry.first)), entry.second,
                       compactNav ? 12.0f + (categoryIndex % navColumns) * ((screen.width - 24.0f) / navSlots) : 12.0f,
                       navY + (compactNav ? (categoryIndex / navColumns) * 48.0f : 0.0f),
                       compactNav ? (screen.width - 24.0f) / navSlots - 4.0f : navWidth - 24.0f, 40.0f,
                       active, metrics.panelFontSize,
                        [&state, category = entry.first] {
                            state.settingsCategory = category;
                            state.settingsScroll = 0.0f;
                            state.fontPicker = FontPickerTarget::None;
                            state.themeListOpen = false;
                            state.associationCustomOpen = false;
                            if (category == SettingsCategory::FileSystem) {
                                refreshFileAssocState(state);
                            }
                            app::requestUpdate();
                       });
            if (active && !compactNav && colors.tokens.metrics.corners == components::theme::CornerStyle::Square) {
                ui.rect("settings.nav.selectionAccent")
                    .position(12.0f, navY + 5.0f).size(2.0f, 30.0f)
                    .color(colors.accent)
                    .hitTestMode(core::dsl::HitTestMode::None).build();
            }
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

                if (colors.tokens.metrics.corners == components::theme::CornerStyle::Square) {
                    ui.rect("settings.contentPane.flatSurface")
                        .fill().color(colors.editor)
                        .hitTestMode(core::dsl::HitTestMode::None).build();
                }

                const float scrollTop = 24.0f;
                const float scrollHeight = std::max(0.0f, pageHeight - scrollTop - 18.0f);
                const float associationSpace = std::max(0.0f, scrollHeight - 160.0f);
                // Compose twice with the same row flow: measure first, then draw.
                // Only drawing registers controls; both passes use identical text metrics.
                const auto mainContent = [&](bool draw) {
                    float rowY = 0;
                    const auto group = [&](const std::string& id, const char* key) {
                        if (rowY > 0) rowY += 16.0f;
                        core::TextStyle title;
                        title.text = i18n::tr(key); title.fontFamily = uiFont;
                        title.fontSize = metrics.panelFontSize + 2; title.fontWeight = 600;
                        title.wrap = true; title.maxWidth = geometry.contentWidth;
                        const float height = core::TextPrimitive::measureTextSize(title).y + 14.0f;
                        if (draw) ui.text(id).position(0, rowY).size(geometry.contentWidth, height)
                            .text(title.text).fontFamily(uiFont).fontSize(title.fontSize).fontWeight(600)
                            .wrap(true).maxWidth(geometry.contentWidth).color(colors.textMuted).build();
                        rowY += height;
                    };
                    const auto row = [&](const std::string& id, const std::string& label,
                                         const std::string& hint, auto control) {
                        const auto measured = settingsRowGeometry(geometry, metrics, uiFont, label, hint);
                        if (draw) {
                            rowLabel(ui, colors, metrics, uiFont, id, label, hint, 0, rowY, labelWidth, measured);
                            control(rowY, measured);
                        }
                        rowY += measured.rowHeight;
                    };
                    if (state.settingsCategory == SettingsCategory::Appearance) {
                        group("settings.group.colors", "settings.group_colors");
                        row("settings.theme", i18n::tr("settings.appearance"), i18n::tr("settings.appearance_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.theme",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.theme_system_short"), i18n::tr("settings.dark"), i18n::tr("settings.light")},
                                        state.themeFollowSystem ? 0 : (state.theme == ThemeMode::Light ? 2 : 1), metrics.panelFontSize,
                                        [&state](int index) {
                                            if (index == 0) {
                                                applyFollowSystemTheme(state);
                                            } else {
                                                applyTheme(state, index == 2 ? ThemeMode::Light : ThemeMode::Dark);
                                            }
                                        });
                            });
                        row("settings.uiStyle", i18n::tr("settings.ui_style"), i18n::tr("settings.ui_style_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.uiStyle",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.ui_style_rounded"), i18n::tr("settings.ui_style_square")},
                                        state.uiStyle == UiStyle::Square ? 1 : 0, metrics.panelFontSize,
                                        [&state](int index) {
                                            applyUiStyle(state, index == 1 ? UiStyle::Square : UiStyle::Rounded);
                                        });
                            });
                        row("settings.themeFile", i18n::tr("settings.theme_file"), state.themeFile.empty()
                                         ? std::string(i18n::tr("settings.theme_builtin_hint"))
                                         : elideToWidth(activeTheme().version >= themeloader::kSchemaVersion2
                                                            ? i18n::format("settings.theme_counts_compact",{
                                                                {"name",activeTheme().name},{"light",std::to_string(editableThemeColorCount(activeTheme().light.colors))},
                                                                {"dark",std::to_string(editableThemeColorCount(activeTheme().dark.colors))}})
                                                            : textfile::fileName(state.themeFile),
                                                        labelWidth, metrics.panelHintFontSize),
                            [&](float rowY, const PanelGeometry& geometry) {
                                slotButton(ui, colors, uiFont, metrics, "settings.themeFile", i18n::tr("settings.theme_open"),
                                       controlX, rowY + geometry.controlTop, controlWidth, !state.themeFile.empty(),
                                       [&state] { openThemeChooser(state); });
                            });
                        group("settings.group.interface", "settings.group_interface");
                        row("settings.scale", i18n::tr("settings.ui_scale"), systemScaleHint(state),
                            [&](float rowY, const PanelGeometry& geometry) {
                                stepControl(ui, colors, "settings.scale",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        scaleText(state),
                                        state.uiScale > kMinimumUiScale + 0.001f,
                                        state.uiScale < kMaximumUiScale - 0.001f,
                                        [&state] { applyUiScale(state, state.uiScale - kUiScaleStep); },
                                        [&state] { applyUiScale(state, state.uiScale + kUiScaleStep); },
                                        uiFont,
                                        metrics.panelFontSize);
                            });
                        row("settings.uiFontSize", i18n::tr("settings.ui_font_size"), i18n::tr("settings.ui_font_size_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                stepControl(ui, colors, "settings.uiFontSize",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        std::to_string(static_cast<int>(std::lround(state.uiFontSize))),
                                        state.uiFontSize > kMinimumUiFontSize + 0.001f,
                                        state.uiFontSize < kMaximumUiFontSize - 0.001f,
                                        [&state] { applyUiFontSize(state, state.uiFontSize - 1.0f); },
                                        [&state] { applyUiFontSize(state, state.uiFontSize + 1.0f); },
                                        uiFont,
                                        metrics.panelFontSize);
                            });
                        rowY += settingsSizePreview(draw ? &ui : nullptr, state, false, geometry, rowY);
                        row("settings.uiFont", i18n::tr("settings.ui_font"),
                            state.uiFontFile.empty() ? std::string(i18n::tr("settings.font_default_short")) : fonts::displayNameFor(state.uiFontFile),
                            [&](float rowY, const PanelGeometry& geometry) {
                                slotButton(ui, colors, uiFont, metrics, "settings.uiFont", i18n::tr("settings.fonts_action"),
                                    controlX, rowY + geometry.controlTop, controlWidth, !state.uiFontFile.empty(), [&state] {
                                        openFontPicker(state, FontPickerTarget::Ui);
                                    });
                            });
                        group("settings.group.language", "settings.group_language");
                        row("settings.uiLanguage", i18n::tr("menu.ui_language"), i18n::tr("settings.language_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui,colors,"settings.uiLanguage",controlX,rowY+geometry.controlTop,controlWidth,
                                {i18n::tr("settings.language_auto"),"简体中文","English"},i18n::preference()=="zh-CN"?1:i18n::preference()=="en"?2:0,metrics.panelFontSize,
                                [&state](int index){applyUiLanguage(state,index==1?"zh-CN":index==2?"en":"system");});
                            });
                        row("settings.animations", i18n::tr("settings.animations"), i18n::tr("settings.animations_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.animations",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.on"), i18n::tr("settings.off")}, state.animations ? 0 : 1, metrics.panelFontSize,
                                        [&state](int index) {
                                            state.animations = index == 0;
                                            persistSettings(state);
                                            app::requestUpdate();
                                        });
                            });
                    } else if (state.settingsCategory == SettingsCategory::Editor) {
                        group("settings.group.typography", "settings.group_typography");
                        row("settings.fontSize", i18n::tr("settings.editor_font_size"), i18n::tr("settings.editor_font_size_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                stepControl(ui, colors, "settings.fontSize",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        std::to_string(static_cast<int>(std::lround(state.editorFontSize))),
                                        state.editorFontSize > kMinimumEditorFontSize + 0.001f,
                                        state.editorFontSize < kMaximumEditorFontSize - 0.001f,
                                        [&state] { applyEditorFontSize(state, state.editorFontSize - 1.0f); },
                                        [&state] { applyEditorFontSize(state, state.editorFontSize + 1.0f); },
                                        uiFont,
                                        metrics.panelFontSize);
                            });
                        rowY += settingsSizePreview(draw ? &ui : nullptr, state, true, geometry, rowY);
                        row("settings.bodyFont", i18n::tr("settings.body_font"),
                            state.editorFontFile.empty() ? std::string(i18n::tr("settings.font_default_short")) : fonts::displayNameFor(state.editorFontFile),
                            [&](float rowY, const PanelGeometry& geometry) {
                                slotButton(ui, colors, uiFont, metrics, "settings.bodyFont", i18n::tr("settings.fonts_action"),
                                    controlX, rowY + geometry.controlTop, controlWidth, !state.editorFontFile.empty(), [&state] {
                                        openFontPicker(state, FontPickerTarget::Editor);
                                    });
                            });
                        row("settings.codeFont", i18n::tr("settings.code_font"),
                            state.codeFontFile.empty() ? std::string(i18n::tr("settings.font_default_short")) : fonts::displayNameFor(state.codeFontFile),
                            [&](float rowY, const PanelGeometry& geometry) {
                                slotButton(ui, colors, uiFont, metrics, "settings.codeFont", i18n::tr("settings.fonts_action"),
                                    controlX, rowY + geometry.controlTop, controlWidth, !state.codeFontFile.empty(), [&state] {
                                        openFontPicker(state, FontPickerTarget::Code);
                                    });
                            });
                        row("settings.readableWidth", i18n::tr("settings.readable_width"), i18n::tr("settings.readable_width_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.readableWidth",
                                                                controlX, rowY + geometry.controlTop, controlWidth,
                                                                {i18n::tr("settings.on"), i18n::tr("settings.off")}, state.readableWidth ? 0 : 1,
                                                                metrics.panelFontSize, [&state](int index) {
                                                                    state.readableWidth = index == 0;
                                                                    persistSettings(state);
                                                                    app::requestUpdate();
                                                                });
                            });
                        group("settings.group.workspace", "settings.group_workspace");
                        row("settings.mode", i18n::tr("settings.layout"), i18n::tr("settings.layout_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.mode",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.simple"), i18n::tr("settings.vault")}, state.mode == EditorMode::Vault ? 1 : 0, metrics.panelFontSize,
                                        [&state](int index) {
                                            state.mode = index == 1 ? EditorMode::Vault : EditorMode::Simple;
                                            persistSettings(state);
                                            app::requestUpdate();
                                        });
                            });
                        row("settings.autoCompact", i18n::tr("settings.auto_compact"), i18n::tr("settings.auto_compact_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.autoCompact",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.on"), i18n::tr("settings.off")}, state.autoCompactWindow ? 0 : 1, metrics.panelFontSize,
                                        [&state](int index) {
                                            state.autoCompactWindow = index == 0;
                                            persistSettings(state);
                                            app::requestUpdate();
                                        });
                            });
                        group("settings.group.information", "settings.group_information");
                        row("settings.statusBar", i18n::tr("settings.status_bar"), i18n::tr("settings.status_bar_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.statusBar",
                                        controlX, rowY + geometry.controlTop, controlWidth,
                                        {i18n::tr("settings.show"), i18n::tr("settings.hide")}, state.showStatusBar ? 0 : 1, metrics.panelFontSize,
                                        [&state](int index) {
                                            state.showStatusBar = index == 0;
                                            applyShowStatusBar(state);
                                        });
                            });
                        row("settings.lineNumbers", i18n::tr("settings.line_numbers"), i18n::tr("settings.line_numbers_hint"),
                            [&](float rowY, const PanelGeometry& geometry) {
                                segmentedAt(ui, colors, "settings.lineNumbers",
                                                                controlX, rowY + geometry.controlTop, controlWidth,
                                                                {i18n::tr("settings.show"), i18n::tr("settings.hide")}, state.showLineNumbers ? 0 : 1,
                                                                metrics.panelFontSize, [&state](int index) {
                                                                    state.showLineNumbers = index == 0;
                                                                    persistSettings(state);
                                                                    app::requestUpdate();
                                                                });
                            });
                    } else if (state.settingsCategory == SettingsCategory::FileSystem) {
                        if (draw) fileAssocCard(ui, state, colors, metrics, uiFont, rowY, geometry.contentWidth, associationSpace);
                        rowY += fileAssocCardHeight(metrics, geometry.contentWidth, associationSpace, state) + 12.0f;
                    }
                    if (state.settingsCategory == SettingsCategory::Appearance || state.settingsCategory == SettingsCategory::Editor) {
                        const bool appearance = state.settingsCategory == SettingsCategory::Appearance;
                        group("settings.group.reset", "settings.group_reset");
                        row("settings.resetRow", i18n::tr("settings.reset"),
                            i18n::tr(appearance ? "settings.reset_appearance_hint" : "settings.reset_editor_hint"),
                            [&, appearance](float rowY, const PanelGeometry& geometry) {
                                const auto category = appearance ? SettingsCategory::Appearance : SettingsCategory::Editor;
                                flatButton(ui, colors, "settings.reset",
                                    i18n::tr(appearance ? "settings.reset_appearance_button" : "settings.reset_editor_button"),
                                    controlX, rowY + geometry.controlTop, controlWidth, kControlHeight, false,
                                    metrics.panelFontSize, [&state, category, appearance] {
                                        requestRisk(state,
                                            i18n::tr(appearance ? "settings.reset_appearance_title" : "settings.reset_editor_title"),
                                            i18n::tr(appearance ? "settings.reset_appearance_confirm" : "settings.reset_editor_confirm"),
                                            i18n::tr("settings.reset"), [&state, category] { resetSettingsCategory(state, category); });
                                    }, true);
                            });
                    }
                    return rowY + 10.0f;
                };
                const float contentHeight = mainContent(false);
                const float maxOffset = std::max(0.0f, contentHeight - scrollHeight);
                const float scrollStep = std::max(20.0f, std::min(44.0f, scrollHeight*.45f));
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
                                mainContent(true);
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
