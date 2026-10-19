#pragma once

// EUI-Edits theme JSON v1/v2 and color-only Obsidian CSS import.
// Missing colors use the corresponding built-in palette. JSON retains its
// typography and legacy icon fields; CSS reads colors only (26 workbench fields).
// v2 may contain workbench metadata with immutable import baselines and report.
// read() validates without activation; load() remains the startup compatibility
// path that reads and activates only on success. Source files are never modified.

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

// Read/validate only. Never activates a theme or changes the running application.
bool read(const std::string& utf8Path, ThemeFileData& out, std::string& error);

// Workbench themes retain their original baseline and per-field import report.
std::string serializeWorkbench(const ThemeFileData& current, const ThemeFileData& baseline,
                               const std::string& sourcePath);

// 让一份已解析的主题立即生效：写入 activeTheme() 并推进 themeRevision()，
// editorColors() / markdownStyle() 的缓存下一帧就地重建（不每帧重建）。
void activate(const ThemeFileData& data, const std::string& utf8Path);

// 回到内置配色（同样推进 revision —— 少了这一步，"加载 → 卸载"会撞上同一个
// 版本号，缓存不重建、颜色留在主题上）。
void reset();

// 导出 v2：单侧重载与双侧导出。colors 写全字段表（含历史图标字段），typography 写生效值。
std::string serialize(const std::string& name, bool baseLight,
                      const EditorColors& colors, const ThemeTypography& typography);

// Export both palettes in one v2 file. The single-palette overload also writes v2,
// with the other side absent so it falls back to the built-in palette.
std::string serializeDual(const std::string& name,
                          const EditorColors& light, const ThemeTypography& lightTypography,
                          const EditorColors& dark, const ThemeTypography& darkTypography);

} // namespace neo::themeloader
