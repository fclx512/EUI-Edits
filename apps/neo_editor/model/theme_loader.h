#pragma once

// ── 主题文件：EUI-Edits schema v1/v2 与 Obsidian CSS 导入 ────────────────
//
// 主题文件是**叠在内置配色上的覆盖表**，不是整套替换：缺的字段一律回退到
// model/style_schema.h 的既有值（颜色 = makeColors()，排版 = markdownStyle() 的
// Obsidian 比例）。于是"只改两行的主题文件"也能用，且没有主题文件时输出与
// 改造前逐位一致。解析走仓库自带的 eui/json.h（yyjson 封装），不引第三方依赖；
// UTF-8 路径转原生路径一律走 textfile::pathFromUtf8（GBK 代码页下 fs::path
// 窄串转换会抛 system_error，见提交 5a644a2）。
//
// ── 旧文件格式（version = 1，继续兼容）─────────────────────────────────────
//
//   {
//     "schema": "euiedits-theme",   // 可选；写了就必须是这个值
//     "version": 1,                  // 必填：不认识的版本直接报错，不猜
//     "name": "我的主题",             // 可选：展示名
//     "base": "dark",                // 可选，缺省 "dark"：覆盖叠在哪套内置配色上
//     "colors": {                    // 可选：只写要改的字段，缺的回退 makeColors()
//       "window": "#1B1E21",
//       "markdownAccent": "#8a5cf4ff",
//       "rowHover": [1, 1, 1, 0.055]
//     },
//     "typography": {                // 可选：只写要改的量，缺的回退 markdownStyle()
//       "bodyLineHeight": 1.5,
//       "blockGap": 1.0,
//       "listIndent": 1.625,
//       "radius": 0.25,
//       "codeSizeFactor": 0.875,
//       "bodyFontFamily": "",        // 空串 = 不覆盖（字体是"建议值"，设置优先）
//       "codeFontFamily": ""
//     }
//   }
//
// 字段定义（v1 全集，写错名字会报错而不是静默不生效）：
//   * colors 的 28 个字段：style_schema.h 的 kThemeColorField 表（window/panel/
//     toolbar/editor/statusBar/text/textMuted/border/accent/rowHover/rowActive/
//     codeBackground/pressed/heading/codeText/quoteBackground/divider/
//     markdownAccent/tokenKeyword..tokenProperty/iconFolder/iconFile/iconMd）。
//     取值 = "#RRGGBB" / "#RRGGBBAA"（大小写不敏感），或 0..1 浮点的
//     [r,g,b] / [r,g,b,a] 数组。tokens 是派生量，不接受直接覆盖（会被重算）。
//   * typography 的 7 个字段：bodyLineHeight / blockGap / listIndent / radius /
//     codeSizeFactor（倍率，缺省值 = kMarkdown* 常量，合法区间见
//     theme_loader.cpp 的 kNumberFields）、bodyFontFamily / codeFontFamily
//     （非空字符串；空串 = 不覆盖，设置里自选过字体时也会被忽略）。
//     **标题相关的排版不开放**：h1/h2/h3 字号倍率与 h?LineHeight（含 1em 上方
//     留白）在 v1 不可由主题改写。
//   * v1 的 base = "dark" | "light"：只覆盖一侧，另一侧保持内置。
//     加载 v1 文件时应用会把外观切到 base 对应的一侧。
//
// v2 用 {"schema":"euiedits-theme","version":2,"name":"...",
// "light":{"colors":{...},"typography":{...}},
// "dark":{"colors":{...},"typography":{...}}} 表示两套独立覆盖；至少有一侧。
// 每侧缺失的字段回退对应的内置配色。v2 typography 除 v1 字段外还支持
// h1/2/3SizeFactor、h1/2/3LineHeightFactor、headingSpaceBefore、
// tableSpaceBefore。导出写 v2，读取 v1 的行为仍按旧版处理。
//
// load() 还接受 Obsidian manifest.json（同目录需有 theme.css）或直接的
// theme.css。只摄取根与 .theme-light/.theme-dark 的变量声明；映射不了的颜色
// 留空并回退内置值，可用 light/dark.colors.size() 查看 28 色覆盖数。
//
// 失败语义：文件不存在、读失败、JSON 损坏、版本不认识、字段名/取值不合法 ——
// 一律返回 false + 人话 error，**不抛异常、不改变当前生效的配色**（调用方维持
// 原状即可，启动路径因此绝不会因为坏主题起不来）。

#include "model/style_schema.h"

#include <string>

namespace neo::themeloader {

// 版本号与 schema 标识。
inline constexpr int kSchemaVersion = 1;
inline constexpr int kSchemaVersion2 = 2;
inline constexpr const char* kSchemaId = "euiedits-theme";

// 解析一段 EUI-Edits 主题 JSON（不生效）。成功时 out 拿到覆盖项。
bool parse(const std::string& json, ThemeFileData& out, std::string& error);

// 读 JSON、Obsidian manifest 或 CSS + 解析 + 立即生效。utf8Path 是 UTF-8。
// 失败不动当前生效主题。
bool load(const std::string& utf8Path, std::string& error);

// 让一份已解析的主题立即生效：写入 activeTheme() 并推进 themeRevision()，
// editorColors() / markdownStyle() 的缓存下一帧就地重建（不每帧重建）。
void activate(const ThemeFileData& data, const std::string& utf8Path);

// 回到内置配色（同样推进 revision —— 少了这一步，"加载 → 卸载"会撞上同一个
// 版本号，缓存不重建、颜色留在主题上）。
void reset();

// 导出 v2：单侧重载与双侧导出。colors 写全 28 个字段，typography 写生效值。
std::string serialize(const std::string& name, bool baseLight,
                      const EditorColors& colors, const ThemeTypography& typography);

// Export both palettes in one v2 file. The single-palette overload also writes v2,
// with the other side absent so it falls back to the built-in palette.
std::string serializeDual(const std::string& name,
                          const EditorColors& light, const ThemeTypography& lightTypography,
                          const EditorColors& dark, const ThemeTypography& darkTypography);

} // namespace neo::themeloader
