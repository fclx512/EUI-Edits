#pragma once

#include <string>
#include <vector>

#include "model/text_file.h"

namespace neo::settings {

// 只有跨会话需要记住的偏好；窗口尺寸不存，避免把最大化尺寸记成常规尺寸。
struct Data {
    std::string vault;
    std::string lastFile;
    // 最近成功打开或另存为的本地文件，最近使用的排在最前，最多 8 条。
    std::vector<std::string> recentFiles;
    int mode = 0;
    bool showStatusBar = true;
    float vaultWidth = 264.0f;
    // 界面缩放。1.0 表示"跟随系统"：框架已经把系统 DPI 乘进渲染缩放
    // （effectiveScale = dpiScale * uiScale），这里再大一点就是在其之上额外放大。
    // 不要用它去"补偿"系统缩放，那会变成双重放大。
    float uiScale = 1.0f;
    // UI language is independent of document syntax mode.
    std::string uiLanguage = "system";
    // 编辑区字号（逻辑像素）。默认值按"不小于记事本正文"取。
    float editorFontSize = 16.0f;
    // 界面字号（菜单/状态栏/侧栏/设置面板）。原先这些尺寸是散落的字面量，
    // 改设置里的"字号"只影响编辑区，界面一点不动——这一项就是那个缺口。
    float uiFontSize = 14.0f;
    // 自选字体文件（完整路径，空 = 用预设）。之所以存路径而不是字体名：
    // 框架对"含 `.` 的字符串"直接按文件路径加载（`core/render/text.cpp:1850-1852`）。
    // 编辑区/预览用 editorFontFile，菜单/按钮/对话框等所有界面文字用 uiFontFile，
    // 代码块/行内代码的等宽字面用 codeFontFile（空 = 系统等宽预设）。
    std::string editorFontFile;
    std::string uiFontFile;
    std::string codeFontFile;
    // 界面主题：0 = 暗色，1 = 亮色，2 = 跟随系统（默认）。跟随系统在应用时
    // 按 Windows 个人化的 AppsUseLightTheme 解析成 0/1；settings.ini 里没有
    // theme 键（首次运行）时也落在这里，之后以用户的选择为准。
    int theme = 2;
    // 主题文件（T13）：UTF-8 绝对路径，空 = 用内置配色。落盘 key 是
    // last_theme_file，开机由 app.cpp 在首帧配色之前恢复。
    std::string lastThemeFile;
    // 编辑区行号列。
    bool lineNumbers = true;
    // 限制行宽（Obsidian 的"可读行宽"）：内容列收窄居中。
    bool readableWidth = true;
    // 界面动画（悬停/按压配色过渡、按压缩放等反馈动画）。落盘 key 是
    // animations。首次运行（settings.ini 里还没有这个键）跟随 Windows 的
    // "辅助功能 → 视觉效果 → 动画效果"；之后一律以用户的选择为准。
    bool animations = true;
    // 粘贴图片的附件目录模式（R2）：0 = shared（文档旁 _assets/，默认），
    // 1 = per-file（<文档名>.assets/）。其他值按 0 处理。
    int attachmentMode = 0;
};

std::string configDirectory();

// "跟随系统"主题的探测：读 HKCU\...\Themes\Personalize 的 AppsUseLightTheme
// （应用模式明暗）。键缺失或读取失败按亮色处理。
bool systemThemePrefersLight();

// 首次访问时从磁盘读取，之后返回同一份可写数据。
Data& current();

// 把 current() 写回磁盘。
bool flush();

// 未保存内容的应急副本：内容变化时定期落盘，保存成功后清除。
// 框架没有“关窗前拦截”的钩子，这是避免误关丢内容的兜底。
//
// 格式分两代：
//   旧 `#neo-recovery <origin>`：只有 UTF-8 正文与来源路径（不带编码元数据）。
//   新 `#neo-recovery-v2 {"origin":...,"encoding":...,"codepage":N,"bom":B,"crlf":B}`：
//   元数据为单行 JSON（编码/页码/BOM/换行），恢复时能按原编码还原 Document；
//   元数据缺失、超长或字段非法时拒绝恢复，不冒认旧格式。
// doc == nullptr 时写旧格式（兼容既有测试与外部读取方）；应用层全部传 doc。
bool writeRecovery(const std::string& text, const std::string& originPath,
                   const textfile::Document* doc = nullptr);
bool writeRecovery(const std::string& text, const std::string& originPath,
                   const textfile::Document* doc, const std::string& language);

struct RecoverySnapshot {
    std::string text;         // 正文，UTF-8 + LF
    std::string originPath;
    std::string language; // optional presentation for an unsaved/recovered document
    bool hasMeta = false;     // true = 新格式，doc 的编码字段可信
    textfile::Document doc;   // hasMeta 时 encoding/bom/crlf 已还原；旧格式保持默认
};

bool readRecovery(RecoverySnapshot& out);
// 兼容形态：只取正文与来源路径，忽略编码元数据。
bool readRecovery(std::string& text, std::string& originPath);
void clearRecovery();

} // namespace neo::settings
