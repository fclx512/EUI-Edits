#pragma once

// 样式模式（T10 拆层）：主题配色、排版比例、字体预设的**纯样式定义**。
//
// 拆层动机：lp_decorations（model 层）原来为了拿 EditorColors / ThemeMode /
// markdownStyle 而 include 了状态层的头 —— model 因此反向依赖整个应用状态。
// 这里把这些"纯样式数据 + 纯样式函数"收拢成一个不依赖状态层的头：
//   * 只吃参数（配色表、字号、字体名），不查 state()；
//   * 状态层 include 本文件，查 state() 的便利包装（editorColors()、按状态取
//     字体名的重载）继续留在状态层；
//   * 于是 model 层 include 本文件即可，不必认识应用状态（见 lp_decorations.h）。
//
// 取值与公式从状态层**逐字搬来**：两套配色、Obsidian 排版比例
// （标题间距上置的 h?LineHeight = 1.2×字号 + em 等）、字体预设一个都没改。

#include "components/markdown.h"  // components::MarkdownStyle（markdownStyle 的返回类型）
#include "eui/types.h"            // eui::Color（EditorColors 的字段类型）

#include <cstring>
#include <map>
#include <optional>
#include <string>

namespace neo {

// 框架自带默认字体是 Bold 的显示体，界面文字统一换成系统正文字体。
inline constexpr const char* kUiFontFamily = "Microsoft YaHei";

// 界面主题。两套配色都在 makeColors() 里，视图层只认 EditorColors 的语义字段，
// 所以加一套配色不用改任何绘制代码。
enum class ThemeMode { Dark = 0, Light = 1 };

// 全部配色集中在这里。框架没有样式表也没有热重载，换肤就是改这个函数。
struct EditorColors {
    eui::Color window;
    eui::Color panel;
    eui::Color toolbar;
    eui::Color editor;
    eui::Color statusBar;
    eui::Color text;
    eui::Color textMuted;
    eui::Color border;
    eui::Color accent;
    eui::Color rowHover;
    eui::Color rowActive;
    eui::Color codeBackground;
    // 按钮/可点行的"按下"叠加色。深色主题下要提亮、浅色主题下要压暗，
    // 所以必须进配色表——写死 rgba(1,1,1,0.1) 在浅色下等于看不见。
    eui::Color pressed;
    // 预览/编辑区的语义色。放在这里而不是写死在 markdownStyle 里，
    // 是为了让主题切换只改一处。
    eui::Color heading;         // 标题文字
    eui::Color codeText;        // 代码块 / 行内代码的文字
    eui::Color quoteBackground; // 引用块底纹
    eui::Color divider;         // 分隔线
    // Markdown 主题色（Obsidian accent：hsl(258,88%,66%) 的紫）。只用于正文语义
    // （链接、引用竖条、任务勾选框），界面控件的 accent 仍是蓝色——两套分开，
    // 否则"复刻 Obsidian 正文观感"会连菜单/按钮一起染色。
    eui::Color markdownAccent;
    // 代码块语法 token 八色里的七色（Obsidian 规格表 §4；"def/普通标识符"不单独
    // 存 —— 就是 codeText，Plain 不产生 run）。随主题两套。
    eui::Color tokenKeyword;
    eui::Color tokenString;
    eui::Color tokenNumber;
    eui::Color tokenComment;
    eui::Color tokenOperator;
    eui::Color tokenFunction;
    eui::Color tokenProperty;
    // 图标色。功能符号由应用内矢量路径绘制，尺寸来自 UiMetrics。
    // 文档图标与 Windows 外壳图标共用一套配色：md 紫、txt 蓝、其余中性灰。
    eui::Color iconFolder;
    eui::Color iconFile;
    eui::Color iconMd;
    eui::Color iconTxt;
    eui::Color iconCode;
    eui::Color iconData;
    components::theme::ThemeColorTokens tokens;
};

inline eui::Color rgba(float red, float green, float blue, float alpha = 1.0f) {
    return eui::Color{red, green, blue, alpha};
}

// ── 主题文件（T13 第一阶段）──────────────────────────────────────────────
// 主题 JSON（v1/v2，完整说明在 model/theme_loader.h）是**叠在内置配色上的
// 覆盖表**，不是整套替换：colors 只写要改的语义色，typography 只写要改的排版量，
// 任何缺字段都回退到本文件 makeColors() / markdownStyle() 的既有值。

// markdownStyle 的排版倍率默认值 —— 就是 2026-09-25 定稿的 Obsidian 比例，
// 也是主题 typography 字段的回退值（字段缺省 ⇒ 下面 value_or 拿到的就是它，
// 于是没有主题文件时输出与改造前逐位一致）。
inline constexpr float kMarkdownBodyLineHeight = 1.5f;    // 正文行高 = 1.5 × 字号
inline constexpr float kMarkdownBlockGap = 1.0f;          // 段间距 = 1rem
inline constexpr float kMarkdownListIndent = 1.625f;      // 列表缩进
inline constexpr float kMarkdownRadius = 0.25f;           // 圆角 --radius-s 4px @ 16
inline constexpr float kMarkdownCodeSizeFactor = 0.875f;  // 代码字号 = 正文 × 0.875
inline constexpr float kMarkdownCodeLineHeight = 1.5f;    // 代码行高 = 代码字号 × 1.5
// 标题的排版倍率在 v1 固定；v2 可按明暗分别覆盖。

// 主题文件 typography 段的覆盖项。std::optional = 该字段没写 ⇒ 用上面的默认值。
// 字体名是"建议值"：设置里自选过字体时仍以设置为准（见 markdownStyle）。
struct ThemeTypography {
    std::optional<float> bodyLineHeight;
    std::optional<float> blockGap;
    std::optional<float> listIndent;
    std::optional<float> radius;
    std::optional<float> codeSizeFactor;
    // v2: heading sizes, line boxes and block spacing are ratios to the body font.
    std::optional<float> h1SizeFactor;
    std::optional<float> h2SizeFactor;
    std::optional<float> h3SizeFactor;
    std::optional<float> h1LineHeightFactor;
    std::optional<float> h2LineHeightFactor;
    std::optional<float> h3LineHeightFactor;
    std::optional<float> headingSpaceBefore;
    std::optional<float> tableSpaceBefore;
    std::optional<std::string> bodyFontFamily;
    std::optional<std::string> codeFontFamily;
};

struct ThemeVariant {
    std::map<std::string, eui::Color> colors;
    ThemeTypography typography;
};

// 当前生效的主题文件内容。空 path = 没有主题文件（内置配色）。
// v1 使用 colors/typography/baseLight；v2 使用 light/dark。
//
// 这份数据放在头文件的 inline 存储里（C++17 内联变量/内联函数静态量：全程序唯一
// 实例），而不是由 theme_loader.cpp 持有再开访问函数：style_schema.h 不能反向依赖
// theme_loader.cpp —— 只编头文件的单测（lp_decorations / undo_incremental /
// perf_benchmark）会因此断链。写入方只有 model/theme_loader.cpp。
struct ThemeFileData {
    int version = 0;          // schema 版本（0 = 没有生效的主题文件）
    std::string name;         // 主题名（展示用，可空）
    bool baseLight = false;   // 覆盖叠在哪套内置配色上：false = 暗色，true = 亮色
    std::map<std::string, eui::Color> colors;   // 仅覆盖项（字段名 → 颜色）
    ThemeTypography typography;                 // 仅覆盖项
    ThemeVariant light;                         // v2: independent light overrides
    ThemeVariant dark;                          // v2: independent dark overrides
    int skippedCssDeclarations = 0;             // Obsidian import report
    std::string path;                          // UTF-8 主题文件路径（空 = 内置）
    // 生效内容的版本号：每次加载/卸载主题 ++。editorColors() 拿它当缓存键
    // （命中 = 一次整数比较；变了 = 就地重建一次两套配色）。markdownStyle 不缓存，
    // 它本来就是每次现算的，直接读下面的覆盖项即可。
    unsigned long long revision = 0;
};

inline ThemeFileData& activeTheme() {
    static ThemeFileData value;
    return value;
}

// 当前主题版本（装饰层缓存键接线要用的那一个：lp 的 DecorationCache 还没吃它，
// 见 model/lp_decorations.h 的 themeKey —— 那是 T13 剩余项，本阶段不改）。
inline unsigned long long themeRevision() {
    return activeTheme().revision;
}

// EditorColors 的可主题化字段表（颜色段的字段名 → 成员指针）。
// 解析、校验、导出三处共用这一张表，加字段只改这里 —— 名字写错会在解析时报错，
// 不会变成"静默不生效的配置"。
struct ThemeColorField {
    const char* name;
    eui::Color EditorColors::* member;
};

inline constexpr ThemeColorField kThemeColorFields[] = {
    {"window", &EditorColors::window},
    {"panel", &EditorColors::panel},
    {"toolbar", &EditorColors::toolbar},
    {"editor", &EditorColors::editor},
    {"statusBar", &EditorColors::statusBar},
    {"text", &EditorColors::text},
    {"textMuted", &EditorColors::textMuted},
    {"border", &EditorColors::border},
    {"accent", &EditorColors::accent},
    {"rowHover", &EditorColors::rowHover},
    {"rowActive", &EditorColors::rowActive},
    {"codeBackground", &EditorColors::codeBackground},
    {"pressed", &EditorColors::pressed},
    {"heading", &EditorColors::heading},
    {"codeText", &EditorColors::codeText},
    {"quoteBackground", &EditorColors::quoteBackground},
    {"divider", &EditorColors::divider},
    {"markdownAccent", &EditorColors::markdownAccent},
    {"tokenKeyword", &EditorColors::tokenKeyword},
    {"tokenString", &EditorColors::tokenString},
    {"tokenNumber", &EditorColors::tokenNumber},
    {"tokenComment", &EditorColors::tokenComment},
    {"tokenOperator", &EditorColors::tokenOperator},
    {"tokenFunction", &EditorColors::tokenFunction},
    {"tokenProperty", &EditorColors::tokenProperty},
    {"iconFolder", &EditorColors::iconFolder},
    {"iconFile", &EditorColors::iconFile},
    {"iconMd", &EditorColors::iconMd},
    {"iconTxt", &EditorColors::iconTxt},
    {"iconCode", &EditorColors::iconCode},
    {"iconData", &EditorColors::iconData},
};

// tokens（组件主题令牌）不在表里：它是上面这些语义色的派生量，见 deriveThemeTokens。
inline const ThemeColorField* findThemeColorField(const std::string& name) {
    for (const ThemeColorField& field : kThemeColorFields) {
        if (name == field.name) {
            return &field;
        }
    }
    return nullptr;
}

// 组件主题令牌（tokens）：由下面几条语义色**派生**，所以主题覆盖改完语义色要
// 重算一次（直接写死一份会留下"控件还画着旧色"的缝）。抽成函数就是为了给
// makeColors 与 buildEditorColors 共用同一条派生路径。
inline void deriveThemeTokens(EditorColors& value, bool light) {
    auto tokens = light ? components::theme::light() : components::theme::dark();
    tokens.background = value.window;
    tokens.surface = value.toolbar;
    tokens.surfaceHover = value.rowHover;
    tokens.surfaceActive = value.pressed;
    tokens.text = value.text;
    tokens.border = value.border;
    tokens.primary = value.accent;
    value.tokens = tokens;
}

// 两套配色的唯一产地。`light == false` 是暗色（默认，也是 2026-09-22 定稿的那一套）。
//
// 浅色这一套的取值参照 GitHub / VS Code 的浅色中性色：
// 底 #FFFFFF、面板 #F7F8FA、边框 #D0D7DE、正文 #1F2328、次要文字 #6E7781、
// 主色沿用框架的 defaultPrimary(56,113,224)；代码块底 #F2F4F7（比正文底略暗一档）。
inline EditorColors makeColors(bool light) {
    EditorColors value;
    if (light) {
        value.window = rgba(0.937f, 0.941f, 0.949f);
        value.editor = rgba(1.000f, 1.000f, 1.000f);
        value.panel = rgba(0.969f, 0.973f, 0.980f);
        value.toolbar = rgba(0.941f, 0.945f, 0.957f);
        value.statusBar = rgba(0.918f, 0.925f, 0.937f);
        value.text = rgba(0.122f, 0.137f, 0.157f);
        value.textMuted = rgba(0.431f, 0.467f, 0.506f);
        value.border = rgba(0.816f, 0.843f, 0.871f);
        value.accent = rgba(0.220f, 0.443f, 0.878f);
        // hsl(210, 4.8%, 91.8%): an opaque neutral hover on every light surface.
        value.rowHover = rgba(0.914064f, 0.918f, 0.921936f);
        value.rowActive = rgba(0.220f, 0.443f, 0.878f, 0.140f);
        value.codeBackground = rgba(0.953f, 0.961f, 0.973f);
        value.pressed = rgba(0.0f, 0.0f, 0.0f, 0.070f);
        value.heading = rgba(0.051f, 0.067f, 0.090f);
        value.codeText = rgba(0.133f, 0.133f, 0.133f);      // Obsidian --code-normal #222222
        value.quoteBackground = rgba(0.220f, 0.443f, 0.878f, 0.070f);
        value.divider = rgba(0.847f, 0.871f, 0.894f);
        // Obsidian accent-1（interactive-accent 的浅色侧计算值，实测 #9873F7）。
        value.markdownAccent = rgba(0.596f, 0.451f, 0.969f);
        // 语法 token 八色（规格表 §4 浅色列）。
        value.tokenKeyword = rgba(0.835f, 0.224f, 0.518f);  // #d53984
        value.tokenString = rgba(0.118f, 0.478f, 0.251f);
        value.tokenNumber = rgba(0.471f, 0.322f, 0.933f);   // #7852ee
        value.tokenComment = rgba(0.431f, 0.467f, 0.506f);
        value.tokenOperator = rgba(0.914f, 0.192f, 0.278f); // #e93147
        value.tokenFunction = rgba(0.584f, 0.365f, 0.075f);
        value.tokenProperty = rgba(0.000f, 0.459f, 0.478f);
        value.iconFolder = rgba(0.431f, 0.467f, 0.506f);
        value.iconFile = rgba(0.486f, 0.569f, 0.702f); // paper outline #7c91b3
        // 与 Windows 外壳图标同一组强调色：#7962ce 紫 / #397bd1 蓝。
        value.iconMd = rgba(0.475f, 0.384f, 0.808f);
        value.iconTxt = rgba(0.224f, 0.482f, 0.820f);
        value.iconCode = rgba(0.086f, 0.537f, 0.475f); // #168879 source
        value.iconData = rgba(0.831f, 0.482f, 0.145f); // #d47b25 structured data
    } else {
        value.window = rgba(0.106f, 0.118f, 0.133f);
        value.editor = rgba(0.118f, 0.133f, 0.149f);
        value.panel = rgba(0.098f, 0.110f, 0.125f);
        value.toolbar = rgba(0.086f, 0.098f, 0.112f);
        value.statusBar = rgba(0.082f, 0.094f, 0.106f);
        value.text = rgba(0.878f, 0.894f, 0.914f);
        value.textMuted = rgba(0.549f, 0.580f, 0.616f);
        value.border = rgba(0.180f, 0.200f, 0.227f);
        value.accent = rgba(0.322f, 0.590f, 1.000f);
        value.rowHover = rgba(0.190f, 0.210f, 0.235f);
        value.rowActive = rgba(0.322f, 0.590f, 1.000f, 0.180f);
        value.codeBackground = rgba(0.086f, 0.098f, 0.114f);
        value.pressed = rgba(1.0f, 1.0f, 1.0f, 0.100f);
        value.heading = rgba(0.945f, 0.957f, 0.972f);
        value.codeText = rgba(0.855f, 0.855f, 0.855f);      // Obsidian --code-normal #dadada
        value.quoteBackground = rgba(0.322f, 0.590f, 1.000f, 0.100f);
        value.divider = rgba(0.220f, 0.243f, 0.278f);
        // Obsidian interactive-accent（暗色直出 hsl(258,88%,66%)）。
        value.markdownAccent = rgba(0.540f, 0.361f, 0.958f);
        // 语法 token 八色（规格表 §4 暗色列）。
        value.tokenKeyword = rgba(0.980f, 0.600f, 0.804f);  // #fa99cd
        value.tokenString = rgba(0.267f, 0.812f, 0.431f);   // #44cf6e
        value.tokenNumber = rgba(0.659f, 0.510f, 1.000f);   // #a882ff
        value.tokenComment = rgba(0.549f, 0.580f, 0.616f);
        value.tokenOperator = rgba(0.984f, 0.275f, 0.298f); // #fb464c
        value.tokenFunction = rgba(0.878f, 0.871f, 0.443f); // #e0de71
        value.tokenProperty = rgba(0.325f, 0.875f, 0.867f); // #53dfdd
        // 图标色比正文暗一档，避免侧栏里图标抢文字；.md/.txt 用外壳图标的紫/蓝做区分。
        value.iconFolder = rgba(0.478f, 0.522f, 0.573f);
        value.iconFile = rgba(0.510f, 0.584f, 0.690f); // paper outline #8295b0
        value.iconMd = rgba(0.647f, 0.549f, 0.961f);
        value.iconTxt = rgba(0.435f, 0.659f, 0.941f);
        value.iconCode = rgba(0.282f, 0.667f, 0.604f);
        value.iconData = rgba(0.902f, 0.631f, 0.353f);
    }

    deriveThemeTokens(value, light);
    return value;
}

// 主题文件生效时的配色：内置配色打底 + 主题的 colors 覆盖 + 重算 tokens。
// v1 仅叠加 base 对应的一侧；v2 分别叠加 light/dark。
inline EditorColors buildEditorColors(bool light) {
    EditorColors value = makeColors(light);
    const ThemeFileData& theme = activeTheme();
    if (theme.path.empty()) {
        return value;
    }
    const std::map<std::string, eui::Color>* overrides = nullptr;
    if (theme.version >= 2) {
        overrides = light ? &theme.light.colors : &theme.dark.colors;
    } else if (theme.baseLight == light) {
        overrides = &theme.colors;
    }
    if (!overrides || overrides->empty()) {
        return value;
    }
    for (const auto& entry : *overrides) {
        if (const ThemeColorField* field = findThemeColorField(entry.first)) {
            value.*field->member = entry.second;
        }
    }
    deriveThemeTokens(value, light);
    return value;
}

// 指定主题的配色。**版本号缓存**（T13 改）：命中 = 每次调用一次整数比较，不构造
// 任何东西（每帧几千次调用的开销上限）；主题文件加载/卸载会推进 themeRevision()，
// 下一次调用把两套配色就地重建一次 —— 既不会每帧新建颜色，也不会像旧版那样用
// 函数级 static const 把**首次**的颜色锁死到进程结束（旧版加载主题文件后，
// editorColors() 永远返回启动那一刻的内置色）。
// 深浅切换不需要重建：两套配色本来就同时在缓存里，切枚举即可（立即生效）。
// 带参版给"按指定主题取色"的调用方（装饰缓存构建在主题切换帧之前）。
inline const EditorColors& editorColors(ThemeMode mode) {
    struct Cache {
        // 与 themeRevision() 的初值（0）必然不同 ⇒ 首次调用一定先建一次。
        unsigned long long revision = ~0ULL;
        EditorColors dark;
        EditorColors light;
    };
    static Cache cache;
    const unsigned long long revision = themeRevision();
    if (cache.revision != revision) {
        cache.dark = buildEditorColors(false);
        cache.light = buildEditorColors(true);
        cache.revision = revision;
    }
    return mode == ThemeMode::Light ? cache.light : cache.dark;
}

// Markdown 预览 / Live Preview 主题。字号不写死：全部从编辑区字号按比例派生，
// 预览和编辑区就不会出现"改了字号一边跟一边不跟"。
//
// 排版数值来源（2026-09-25，参考/obsidian-style/Obsidian样式规格.md）：Obsidian 默认
// 主题（app.css 源码 + 运行中实例 CDP 实测双重确认）。基准 16px 下的关键值：
// 正文行高 1.5（24px）；h1/h2/h3 = 1.618/1.462/1.318em、行盒 1.2 倍字号、标题行
// 上方另有 16px（--heading-spacing 的 LP 近似，见下）；代码 0.875em、行高 1.5×代码字号；
// 段间距 1rem；代码块/行内代码圆角 4px；表格 cell padding 4px 8px、表内字号 = 正文。
// 这里取**比例**（em），用户改字号时观感保持一致。
//
// 配色作为参数传入（T10 拆层）：本函数纯、不查 state()，主题由调用方给 ——
// UI 编排层传 editorColors(state.theme)，单测传 editorColors(ThemeMode::Dark/Light)。
//
// 排版覆盖：v1 读取单侧 typography；v2 读取指定明暗侧的 typography。
// 缺字段回退上面的默认倍率。v1 不接受标题字段，v2 支持。
inline components::MarkdownStyle markdownStyle(float bodySize, const char* bodyFontFamily,
                                               const char* codeFontFamily,
                                               const EditorColors& colors,
                                               ThemeMode mode = ThemeMode::Dark) {
    const ThemeFileData& theme = activeTheme();
    const ThemeTypography& typography = theme.version >= 2
        ? (mode == ThemeMode::Light ? theme.light.typography : theme.dark.typography)
        : theme.typography;
    components::MarkdownStyle style(colors.tokens);
    // 颜色全部从配色表取（含主题）：这里不再有任何写死的 rgba，
    // 否则切到浅色主题时会留下几处"暗色专属"的文字色。
    style.text = colors.text;
    style.heading = colors.heading;
    style.muted = colors.textMuted;
    style.accent = colors.markdownAccent;
    style.codeText = colors.codeText;
    style.codeBackground = colors.codeBackground;
    style.quoteBackground = colors.quoteBackground;
    style.divider = colors.divider;
    // 正文跟随用户选的编辑区字体；代码块/行内代码跟随代码字体（默认系统等宽预设）。
    // 主题里的字体名只是"建议值"：只有调用方给的还是预设名（= 设置里没自选字体）
    // 才采纳，用户在设置里选过的字体永远优先。
    const char* body = bodyFontFamily != nullptr ? bodyFontFamily : "";
    const char* code = codeFontFamily != nullptr ? codeFontFamily : "monospace";
    if (typography.bodyFontFamily && !typography.bodyFontFamily->empty() &&
        (body[0] == '\0' || std::strcmp(body, kUiFontFamily) == 0)) {
        body = typography.bodyFontFamily->c_str();
    }
    if (typography.codeFontFamily && !typography.codeFontFamily->empty() &&
        (code[0] == '\0' || std::strcmp(code, "monospace") == 0)) {
        code = typography.codeFontFamily->c_str();
    }
    style.fontFamily = body;
    style.codeFontFamily = code;
    // 2026-09-23 起框架的"字号就是 em"（core/render/text.cpp 的 loadFontFace），
    // 所以 em == bodySize，下面的比例与规格表一一对应。
    const float em = bodySize;
    style.bodySize = bodySize;
    style.bodyLineHeight = em * typography.bodyLineHeight.value_or(kMarkdownBodyLineHeight);
    style.h1Size = em * typography.h1SizeFactor.value_or(1.618f);
    style.h2Size = em * typography.h2SizeFactor.value_or(1.462f);
    style.h3Size = em * typography.h3SizeFactor.value_or(1.318f);
    // 标题行盒 = 1.2 × 自身字号（实测 h1 31.07px = 25.888 × 1.2），再加 1em 的
    // 上方留白近似 Obsidian 的标题间距（实测标题行 padding-top 16px；块级
    // spacing 机制 LP 没有，折中进行盒）。**不参与主题覆盖**（见上）。
    style.h1LineHeight = style.h1Size * typography.h1LineHeightFactor.value_or(1.2f) + em;
    style.h2LineHeight = style.h2Size * typography.h2LineHeightFactor.value_or(1.2f) + em;
    style.h3LineHeight = style.h3Size * typography.h3LineHeightFactor.value_or(1.2f) + em;
    style.h1TextLineHeight = style.h1Size * typography.h1LineHeightFactor.value_or(1.2f);
    style.h2TextLineHeight = style.h2Size * typography.h2LineHeightFactor.value_or(1.2f);
    style.h3TextLineHeight = style.h3Size * typography.h3LineHeightFactor.value_or(1.3f);
    style.headingSpaceBefore = em * typography.headingSpaceBefore.value_or(1.0f);
    style.tableSpaceBefore = em * typography.tableSpaceBefore.value_or(1.0f);
    style.codeSize = bodySize * typography.codeSizeFactor.value_or(kMarkdownCodeSizeFactor);
    style.codeLineHeight = style.codeSize * kMarkdownCodeLineHeight;  // 实测 21px = 14 × 1.5
    style.blockGap = em * typography.blockGap.value_or(kMarkdownBlockGap);  // --p-spacing 1rem
    style.listIndent = em * typography.listIndent.value_or(kMarkdownListIndent);
    style.radius = em * typography.radius.value_or(kMarkdownRadius);  // --radius-s 4px @ 16
    return style;
}

// ── 字体名的纯取值（T10 拆层）──────────────────────────────────────────────
// 接 fontFile 字符串（空 = 用预设），不依赖状态层：model 层与单测从这里取；
// 按状态取字体名的便利包装（editorFontFamily(state) 等）留在状态层。

// 编辑区与预览的字体：自选了文件就用它，否则用系统正文字体。
// 代码块/行内代码另有独立的代码字体（见 codeFontFamily），不受这里影响。
inline const char* editorFontFamily(const std::string& fontFile) {
    return fontFile.empty() ? kUiFontFamily : fontFile.c_str();
}

// 代码字体：自选了文件就用它，否则用"monospace"预设 —— 框架会把这个名字解析成
// 系统等宽字体（Windows 上 Cascadia Mono / Consolas，见 core/render/text.cpp）。
// 正文预设是比例字体，代码块用它缩进/对齐全乱，所以等宽是代码块的默认而非可选项。
inline const char* codeFontFamily(const std::string& fontFile) {
    return fontFile.empty() ? "monospace" : fontFile.c_str();
}

} // namespace neo
