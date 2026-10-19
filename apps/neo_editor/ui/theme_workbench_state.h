#pragma once

// 主题工作台的会话状态与纯辅助函数（不画界面）。
#include "model/i18n.h"
#include "model/theme_workbench.h"
#include "model/theme_auto_palette.h"
#include "model/theme_loader.h"
#include "state/app_actions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace neo::theme_workbench_ui {

enum class Layer { Palette, Color, Type };

struct Session {
    bool open = false;
    bool reviewingImport = false, reportDetailsOpen = false;
    bool saveDialog = false, closeDialog = false, closeAfterSave = false, draftMenu = false;
    bool compare = false, codeView = false, light = true;
    bool missingOnly = true, shareType = false, seedEditing = false;
    bool windowClosePending = false, windowCloseApproved = false;
    bool panelHidden = false, catalogRefreshNeeded = false;
    bool hexFocused = false, seedFocused = false, nameFocused = false;
    bool typeSection[3] = {true, true, false};
    Layer layer = Layer::Palette;
    int expandedGroup = -1, trial = -1, autoRound = 0;
    std::size_t active = 0;
    std::vector<themeworkbench::Draft> drafts;
    // 保存收据：同一草稿内容未变时再次保存/应用复用同一文件。
    std::map<std::size_t, std::pair<std::string, std::string>> saveReceipts;
    // 每个草稿"干净"时的序列化内容；不同即视为有未保存修改。空串 = 从未保存过。
    std::map<std::size_t, std::string> cleanState;
    ThemeFileData pendingImport;
    std::string pendingPath, selected = "accent", hovered, message;
    std::string hexDraft, alphaDraft, seedHex = "#62866B", candidateSeed;
    std::vector<ThemeFileData> candidates;
    float panelScroll = 0, typeScroll = 0, previewScroll = 0, previewSidebarScroll = 0, reportScroll = 0;
    // 内嵌取色器：HSV 单独保存，灰色时色相不丢；拖动期间记录命中区域。
    float pickH = 0, pickS = 0, pickV = 0, pickA = 1;
    std::string pickKey;
    int pickMode = 0;  // 0 = 当前颜色项，1 = 基调种子色
    int pickArea = 0;  // 0 = 饱和度/明度，1 = 色相，2 = 不透明度
    bool dragging = false, dragRecorded = false;
    eui::Color pressColor;
    eui::Rect dragRect;
    float menuX = 0, menuY = 0, menuScroll = 0;
};

inline Session& session() { static Session value; return value; }
inline themeworkbench::Draft& draft() { return session().drafts[session().active]; }

inline std::string fieldLabel(const std::string& key) {
    const std::string id = "theme_wb.field." + key;
    return i18n::tr(id.c_str());
}
inline std::string fieldAffects(const std::string& key) {
    const std::string id = "theme_wb.affect." + key;
    return i18n::tr(id.c_str());
}

// 分组顺序即面板中的顺序。
inline const std::vector<std::vector<std::string>>& groups() {
    static const std::vector<std::vector<std::string>> value{
        {"window", "panel", "toolbar", "editor", "statusBar", "border", "divider"},
        {"text", "textMuted", "accent", "selectionBackground", "rowHover", "rowActive", "pressed"},
        {"heading", "markdownAccent", "quoteBackground", "codeBackground", "codeText"},
        {"tokenKeyword", "tokenString", "tokenNumber", "tokenComment", "tokenOperator", "tokenFunction", "tokenProperty"},
    };
    return value;
}
inline int groupOf(const std::string& key) {
    for (std::size_t g = 0; g < groups().size(); ++g)
        if (std::find(groups()[g].begin(), groups()[g].end(), key) != groups()[g].end()) return static_cast<int>(g);
    return 0;
}
inline std::string groupLabel(int g) {
    constexpr const char* ids[] = {"theme_wb.group.surface", "theme_wb.group.text",
                                   "theme_wb.group.markdown", "theme_wb.group.code"};
    return i18n::tr(ids[std::clamp(g, 0, 3)]);
}

inline int byte(float value) { return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255); }
inline std::string hexColor(eui::Color c, bool alpha) {
    char out[10] = {};
    if (alpha) std::snprintf(out, sizeof(out), "#%02X%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    else std::snprintf(out, sizeof(out), "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    return out;
}
// 接受 #RRGGBB 与 #RRGGBBAA（# 可省）。6 位时保留 color 原有的不透明度。
inline bool parseHex(std::string text, eui::Color& color) {
    if (!text.empty() && text[0] == '#') text.erase(0, 1);
    if ((text.size() != 6 && text.size() != 8) ||
        text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    float values[4] = {0, 0, 0, color.a};
    for (std::size_t i = 0; i * 2 < text.size(); ++i)
        values[i] = static_cast<float>(std::stoul(text.substr(i * 2, 2), nullptr, 16)) / 255.0f;
    color = rgba(values[0], values[1], values[2], values[3]);
    return true;
}

struct Hsv { float h = 0, s = 0, v = 0; };
inline Hsv toHsv(eui::Color c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    Hsv o;
    o.v = mx;
    o.s = mx <= 0.0f ? 0.0f : d / mx;
    if (d > 1e-6f) {
        if (mx == c.r) o.h = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) o.h = (c.b - c.r) / d + 2.0f;
        else o.h = (c.r - c.g) / d + 4.0f;
        o.h /= 6.0f;
        if (o.h < 0) o.h += 1.0f;
    }
    return o;
}
inline eui::Color fromHsv(float h, float s, float v, float a = 1.0f) {
    h -= std::floor(h);
    const float x = h * 6.0f, f = x - std::floor(x);
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (static_cast<int>(x) % 6) {
        case 0: return rgba(v, t, p, a);
        case 1: return rgba(q, v, p, a);
        case 2: return rgba(p, v, t, a);
        case 3: return rgba(p, q, v, a);
        case 4: return rgba(t, p, v, a);
        default: return rgba(v, p, q, a);
    }
}
// 选文字色：强调色等实心底上的文字取黑或白，保证可读。
inline eui::Color readableOn(eui::Color bg) {
    const float l = .2126f * bg.r + .7152f * bg.g + .0722f * bg.b;
    return l > .6f ? rgba(.08f, .09f, .11f) : rgba(1, 1, 1);
}

inline bool sameTypography(const ThemeTypography& a, const ThemeTypography& b) {
    return a.bodyLineHeight == b.bodyLineHeight && a.blockGap == b.blockGap && a.listIndent == b.listIndent &&
        a.radius == b.radius && a.codeSizeFactor == b.codeSizeFactor && a.h1SizeFactor == b.h1SizeFactor &&
        a.h2SizeFactor == b.h2SizeFactor && a.h3SizeFactor == b.h3SizeFactor &&
        a.h1LineHeightFactor == b.h1LineHeightFactor && a.h2LineHeightFactor == b.h2LineHeightFactor &&
        a.h3LineHeightFactor == b.h3LineHeightFactor && a.headingSpaceBefore == b.headingSpaceBefore &&
        a.tableSpaceBefore == b.tableSpaceBefore && a.bodyFontFamily == b.bodyFontFamily &&
        a.codeFontFamily == b.codeFontFamily;
}

} // namespace neo::theme_workbench_ui
