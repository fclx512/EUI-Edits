#pragma once

#include "model/atomic_write.h"
#include "model/text_file.h"
#include "model/theme_loader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

namespace neo::themeworkbench {

struct Field {
    const char* name;
    const char* label;
    const char* affected;
    eui::Color EditorColors::* member;
};

inline const std::vector<Field>& fields() {
    static const std::vector<Field> value{
        {"window", "窗口底色", "窗口背景、面板间隙", &EditorColors::window},
        {"panel", "侧栏底色", "文档库、设置侧栏", &EditorColors::panel},
        {"toolbar", "工具栏底色", "主菜单、工具面板", &EditorColors::toolbar},
        {"editor", "正文底色", "编辑区、写作工具栏", &EditorColors::editor},
        {"statusBar", "状态栏底色", "底部状态栏", &EditorColors::statusBar},
        {"text", "正文文字", "段落、侧栏文字、按钮文字", &EditorColors::text},
        {"textMuted", "次要文字", "状态提示、辅助说明", &EditorColors::textMuted},
        {"border", "边框", "面板边界、控件边框", &EditorColors::border},
        {"accent", "界面强调色", "按钮、焦点边框、输入光标", &EditorColors::accent},
        {"selectionBackground", "文本选区", "正文文本选区（颜色与透明度独立）", &EditorColors::selectionBackground},
        {"rowHover", "悬停底色", "侧栏悬停项、菜单悬停项", &EditorColors::rowHover},
        {"rowActive", "选中项底色", "侧栏当前文档、列表当前项", &EditorColors::rowActive},
        {"pressed", "按下底色", "按钮按下、可点行按下", &EditorColors::pressed},
        {"heading", "标题文字", "一级、二级、三级标题", &EditorColors::heading},
        {"markdownAccent", "正文强调色", "链接、引用竖线、任务勾选框", &EditorColors::markdownAccent},
        {"quoteBackground", "引用底纹", "引用块背景", &EditorColors::quoteBackground},
        {"divider", "分隔线", "正文分隔线", &EditorColors::divider},
        {"codeBackground", "代码底色", "代码块、行内代码", &EditorColors::codeBackground},
        {"codeText", "代码文字", "代码块普通文字、行内代码文字", &EditorColors::codeText},
        {"tokenKeyword", "代码关键字", "代码块中的关键字", &EditorColors::tokenKeyword},
        {"tokenString", "代码字符串", "代码块中的字符串", &EditorColors::tokenString},
        {"tokenNumber", "代码数字", "代码块中的数值", &EditorColors::tokenNumber},
        {"tokenComment", "代码注释", "代码块中的注释", &EditorColors::tokenComment},
        {"tokenOperator", "代码运算符", "代码块中的运算符", &EditorColors::tokenOperator},
        {"tokenFunction", "代码函数", "代码块中的函数名", &EditorColors::tokenFunction},
        {"tokenProperty", "代码属性", "代码块中的属性名", &EditorColors::tokenProperty},
    };
    return value;
}

struct Draft {
    ThemeFileData current;
    ThemeFileData baseline;
    std::vector<ThemeFileData> undoStack, redoStack;
    std::string sourcePath, savedPath;
    bool imported = false;
};

inline EditorColors palette(const ThemeFileData& data, bool light) {
    auto value = makeColors(light);
    const auto* overrides = data.version >= 2 ? &(light ? data.light.colors : data.dark.colors)
        : data.baseLight == light ? &data.colors : nullptr;
    if (overrides) for (const auto& entry : *overrides)
        if (const auto* field = findThemeColorField(entry.first)) value.*field->member = entry.second;
    deriveThemeTokens(value, light);
    return value;
}

inline ThemeFileData complete(const ThemeFileData& data) {
    auto result = data;
    for (const bool light : {true, false}) {
        auto& side = light ? result.light : result.dark;
        if (data.version < 2) side.typography = data.baseLight == light ? data.typography : ThemeTypography{};
        const auto p = palette(data, light);
        for (const auto& field : kThemeColorFields) side.colors[field.name] = p.*field.member;
    }
    result.version = 2;
    result.colors.clear();
    result.typography = {};
    return result;
}

inline Draft makeDraft(const ThemeFileData& data, const std::string& sourcePath = "") {
    Draft draft;
    draft.current = complete(data);
    draft.baseline = draft.current;
    draft.sourcePath = data.importSource.empty() ? sourcePath : data.importSource;
    draft.imported = !draft.sourcePath.empty();
    if (data.importLightBaseline && data.importDarkBaseline) {
        draft.savedPath = data.path;
        draft.baseline.light = *data.importLightBaseline;
        draft.baseline.dark = *data.importDarkBaseline;
        draft.baseline = complete(draft.baseline);
    }
    return draft;
}

inline EditorColors colors(const Draft& draft, bool light, bool before = false) {
    return palette(before ? draft.baseline : draft.current, light);
}

inline bool sameColor(eui::Color a, eui::Color b) {
    // File colors have 8-bit channels. A sub-byte difference is not an edit.
    const auto byte = [](float v) { return std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return byte(a.r) == byte(b.r) && byte(a.g) == byte(b.g) && byte(a.b) == byte(b.b) && byte(a.a) == byte(b.a);
}

inline bool changed(const Draft& draft, bool light, const std::string& name) {
    const auto* field = findThemeColorField(name);
    if (!field) return false;
    return !sameColor(colors(draft, light).*field->member, colors(draft, light, true).*field->member);
}

inline std::size_t changedCount(const Draft& draft, bool light) {
    std::size_t count = 0;
    for (const auto& field : fields()) if (changed(draft, light, field.name)) ++count;
    return count;
}

inline void checkpoint(Draft& draft) {
    if (draft.undoStack.size() >= 100) draft.undoStack.erase(draft.undoStack.begin());
    draft.undoStack.push_back(draft.current);
    draft.redoStack.clear();
}

inline bool isLocked(const Draft& draft, bool light, const std::string& name) {
    return (light ? draft.current.lightLocks : draft.current.darkLocks).count(name) != 0;
}

inline bool editableField(const std::string& name) {
    return std::any_of(fields().begin(), fields().end(), [&](const Field& f) { return name == f.name; });
}

inline void setLocked(Draft& draft, bool light, const std::string& name, bool locked) {
    if (!editableField(name) || isLocked(draft, light, name) == locked) return;
    checkpoint(draft);
    auto& locks = light ? draft.current.lightLocks : draft.current.darkLocks;
    if (locked) locks.insert(name); else locks.erase(name);
}

// record=false 用于连续拖动：同一手势只在第一次改动时记撤销点。
inline void setColor(Draft& draft, bool light, const std::string& name, eui::Color color, bool record = true) {
    const auto editable = std::find_if(fields().begin(), fields().end(), [&](const Field& f) { return name == f.name; });
    if (editable == fields().end()) return;
    for (float channel : {color.r, color.g, color.b, color.a})
        if (!std::isfinite(channel) || channel < 0 || channel > 1) return;
    if (sameColor(colors(draft, light).*editable->member, color) && isLocked(draft, light, name)) return;
    if (record) checkpoint(draft);
    (light ? draft.current.light : draft.current.dark).colors[name] = color;
    (light ? draft.current.lightLocks : draft.current.darkLocks).insert(name);
}

inline void restoreColor(Draft& draft, bool light, const std::string& name) {
    if (!editableField(name) || (!changed(draft, light, name) && !isLocked(draft, light, name))) return;
    checkpoint(draft);
    const auto* field = findThemeColorField(name);
    (light ? draft.current.light : draft.current.dark).colors[name] = colors(draft, light, true).*field->member;
    (light ? draft.current.lightLocks : draft.current.darkLocks).erase(name);
}

inline void restoreAll(Draft& draft) {
    checkpoint(draft);
    draft.current.light = draft.baseline.light;
    draft.current.dark = draft.baseline.dark;
    draft.current.lightLocks.clear();
    draft.current.darkLocks.clear();
    draft.current.autoRecipe.reset();
}

inline void adoptGenerated(Draft& draft, const ThemeFileData& generated,
                           const ThemeAutoRecipe& recipe, bool missingOnly = true) {
    checkpoint(draft);
    for (bool light : {true, false}) {
        const auto p = palette(generated, light);
        for (const auto& field : fields()) {
            if (isLocked(draft, light, field.name)) continue;
            const auto& report = draft.current.importReport;
            if (missingOnly && std::any_of(report.begin(), report.end(), [&](const ThemeImportEntry& entry) {
                    return entry.light == light && entry.field == field.name && entry.imported;
                })) continue;
            (light ? draft.current.light : draft.current.dark).colors[field.name] = p.*field.member;
        }
    }
    draft.current.autoRecipe = recipe;
}

inline bool undo(Draft& draft) {
    if (draft.undoStack.empty()) return false;
    draft.redoStack.push_back(draft.current);
    draft.current = std::move(draft.undoStack.back()); draft.undoStack.pop_back(); return true;
}

inline bool redo(Draft& draft) {
    if (draft.redoStack.empty()) return false;
    draft.undoStack.push_back(draft.current);
    draft.current = std::move(draft.redoStack.back()); draft.redoStack.pop_back(); return true;
}

inline void setTypography(Draft& draft, bool light, const ThemeTypography& value, bool record = true) {
    if (record) checkpoint(draft);
    (light ? draft.current.light : draft.current.dark).typography = value;
}

inline void setTypographyBoth(Draft& draft, const ThemeTypography& value, bool record = true) {
    if (record) checkpoint(draft);
    draft.current.light.typography = value;
    draft.current.dark.typography = value;
}

inline bool saveDraft(Draft& draft, const std::string& directory, std::string& error) {
    error.clear();
    const auto dir = textfile::pathFromUtf8(directory);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { error = ec.message(); return false; }
    const auto bytes = themeloader::serializeWorkbench(draft.current, draft.baseline, draft.sourcePath);
    // Round-trip validation prevents publishing a theme the startup loader cannot read.
    ThemeFileData validated;
    if (!themeloader::parse(bytes, validated, error)) return false;
    const auto tick = std::chrono::system_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
        const auto target = dir / ("theme-" + std::to_string(tick) + "-" + std::to_string(i) + ".json");
        if (atomicwrite::writeFileCreateOnly(target, bytes)) {
            draft.savedPath = textfile::pathToUtf8(target);
            draft.current.importLightBaseline = validated.importLightBaseline;
            draft.current.importDarkBaseline = validated.importDarkBaseline;
            draft.current.importSource = draft.sourcePath;
            return true;
        }
        if (!std::filesystem::exists(target, ec)) break;
    }
    error = "无法保存主题，请检查目录写入权限。";
    return false;
}

} // namespace neo::themeworkbench
