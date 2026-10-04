#pragma once

#include "state/app_state.h"

#include <cstddef>
#include <string>
#include <vector>

namespace neo {

// 这里的函数都会做文件 IO 或弹出系统对话框，只能在按钮/快捷键回调里调用，
// 不能放进 compose。

void showToast(AppState& state, std::string title, std::string message);

// 记录已成功打开的本地文件；规范化路径、去重、置顶、限长并立即持久化。
void recordRecentFile(const std::string& path);

struct TabInfo {
    std::uint64_t id;
    std::string name, path, vaultRoot, language;
    bool dirty;
    // 展示身份（非持久化）：由 refreshTabPresentation 解算的色槽与组序号。
    // hasGroup=false 表示该页没有库根（用中性色）；颜色只是提示，root 文字才是身份。
    bool hasGroup = false;
    int colorSlot = 0;
    std::uint64_t groupOrdinal = 0;
};
std::vector<TabInfo> documentTabs(const AppState& state);
// 根文本变化后的动作阶段调用：重新解算展示分组/色槽/组序号（轻量签名短路，不变则 O(n) 比较）。
void refreshTabPresentation(AppState& state);
void clearTabCloseAnchor(AppState& state);

// 顶栏"固定指针连续关闭"的区域判定用：当前窗口有效缩放（GetDpiForWindow/96 × ui_scale）
// 与光标在 client 区的逻辑 DIP 坐标。只在事件/关闭链判定里按需调用，不做高频轮询。
float effectiveWindowScale();
bool cursorClientPosition(float& x, float& y);
bool activateDocumentTab(AppState& state, std::uint64_t id);
void requestCloseTab(AppState& state, std::uint64_t id);
void moveDocumentTab(AppState& state, std::uint64_t id, int direction);
DocumentSession* documentTab(AppState& state, std::uint64_t id);
const DocumentSession* documentTab(const AppState& state, std::uint64_t id);
// Internal transitions: keep modal guards at the public action boundary.
bool createDocumentTab(AppState& state);
void closeActiveDocumentTab(AppState& state);
bool activateDocumentTabInternal(AppState& state, std::uint64_t id);
bool sameDocumentFile(const std::string& left, const std::string& right);
bool documentWithinRoot(const std::string& path, const std::string& root);
bool mergeRelatedVaultRoots(AppState& state);

void newDocument(AppState& state);
bool loadDocument(AppState& state, const std::string& path);
void retryUnavailableDocument(AppState& state);
// Startup preserves a crash recovery draft before honoring an explicit file.
void restoreStartupDocument(AppState& state, const std::string& commandLinePath);
void openFileFromDialog(AppState& state);
void openRecentFile(AppState& state, std::size_t index);

// 有未保存修改时先确认再打开，避免浏览文档库顺手丢掉正在写的内容。
inline bool documentModalOpen(const AppState& state) {
    return state.closeApproved || state.sessionClosePending || state.savePickerOpen || state.pending != PendingAction::None || state.saveConflictOpen ||
        state.riskAction != AppState::RiskAction::None ||
        state.vaultPromptKind != VaultPromptKind::None || state.vaultDeletePending || state.linkEditorOpen ||
        !state.imagePreviewPath.empty() || state.exitClosing;
}
void requestOpenPath(AppState& state, const std::string& path);

bool saveDocument(AppState& state);
bool saveDocumentAs(AppState& state);
bool requestCloseDocument(AppState& state);
void resolveDocumentConfirmation(AppState& state, filesafety::UnsavedChoice choice);
void resolveSaveConflict(AppState& state, int choice); // 0 overwrite, 1 Save As, 2 cancel

// "以编码重新打开"的选项表（文件菜单子项与分发共用同一份，顺序即索引）。
// 非 Windows 平台只含 UTF-8/UTF-16 三项。
struct ReopenEncodingOption {
    const char* label;
    textfile::ForcedEncoding forced;
};
const std::vector<ReopenEncodingOption>& reopenEncodingOptions();

// 按指定编码重新打开当前文档。有未保存修改时先走确认弹层（丢弃后执行）；
// 文档尚未落盘时只提示。重新打开失败保持当前文档不动。
void requestReloadWithEncoding(AppState& state, std::size_t optionIndex);
void performReloadWithEncoding(AppState& state, const textfile::ForcedEncoding& forced);

// 图片粘贴落盘（R2）。pendingImagePaste 置位后的每帧 tick 消费者：读剪贴板位图、
// PNG 编码、附件目录原子落盘，最后排队 InsertImageLink 在 compose 插入链接。
// 只能在主线程调用（touch state/file IO/对话框）。
void pasteImageAsAttachment(AppState& state);

void chooseVaultDirectory(AppState& state);
// 地址栏回车：解析这段文本并跳过去（目录=进入并展开，文件=展开到它并打开）。
void navigateToVaultPath(AppState& state, std::string text);
void refreshVault(AppState& state, bool resetScroll);
// 采纳/请求共享扫描快照（阶段 B）：命中同根快照直接复用，未命中排后台扫描（不阻塞）。
void adoptVaultScan(AppState& state, bool resetScroll);
// 每帧轻量采纳：共享快照代次变了才重建 rows。
void tickVaultScan(AppState& state);
// 请求后台重扫（目录事件/附件落盘后）。
void requestVaultRefresh(AppState& state);
// 干净页激活时排一次后台文件核验（阶段 D），不再在主线程同步读盘。
void scheduleActiveFileVerification(AppState& state);
void toggleFolder(AppState& state, const std::string& relative);
void rebuildRows(AppState& state);

// ── 文档库条目操作（2026-09-26，右键菜单）────────────────────────────────────
// 路径一律用库内相对路径（'/' 分隔）；成功后刷新列表并把新条目滚进视野。
void vaultCreateFile(AppState& state, const std::string& parentRelative, std::string name);
void vaultCreateFolder(AppState& state, const std::string& parentRelative, std::string name);
bool vaultRenameEntry(AppState& state, const std::string& oldRelative, std::string newName,
                      bool allowExtensionChange = false);
void beginVaultRename(AppState& state, const std::string& relative, bool isDirectory,
                      bool returnToRow = false);
void cancelVaultPrompt(AppState& state);
bool confirmVaultRename(AppState& state, bool useSuggestedName = false);
void vaultDeleteEntry(AppState& state, const std::string& relative, bool isDir);

// 链接跳转（Live Preview 单击 / 右键菜单"打开链接"）：http(s)/mailto 交给系统，
// 其余按本地路径解析 —— 文档目录 → 文档库根，没有扩展名先补 .md 再试。
// docDir 为空（未落盘的新文档）时只试文档库根。
void openLinkTarget(AppState& state, std::string target, const std::string& docDir);

// 库内相对路径（'/' 分隔）与绝对路径互转。
std::string vaultRelativePath(const AppState& state, const std::string& absolutePath);
std::string vaultAbsolutePath(const AppState& state, const std::string& relativePath);

void performPending(AppState& state, PendingAction action);
void persistSettings(const AppState& state);

// app.cpp 持有全应用唯一的 DslAppConfig。界面缩放要能在运行时改，
// 所以单独开一个可写入入口；改完必须自己调 app::requestUpdate()，
// 否则逻辑尺寸变了也不会重新 compose。
app::DslAppConfig& mutableAppConfig();

// 界面缩放（settings.ini 的 ui_scale）。1.0 = 跟随系统，调大是在系统缩放之上额外放大。
void applyUiScale(AppState& state, float scale);

// 编辑区字号（settings.ini 的 editor_font_size）。
void applyEditorFontSize(AppState& state, float fontSize);

// 界面字号（settings.ini 的 ui_font_size）。菜单/状态栏/侧栏/设置面板一起变。
void applyUiFontSize(AppState& state, float fontSize);

void applyShowStatusBar(AppState& state);

// 界面主题（settings.ini 的 theme）。配色是全局的：改完不只是重新 compose，
// 还得让保留层里的图元整屏作废 —— 它们的颜色是画的时候烘进去的。
void applyTheme(AppState& state, ThemeMode mode);
// "跟随系统"：立即解析 Windows 应用模式，并由系统外观通知继续更新。
void applyFollowSystemTheme(AppState& state);
// 系统通知只更新跟随偏好；固定亮/暗不受影响，模式没变时不重绘或落盘。
void refreshSystemTheme(AppState& state);

// 自选字体文件（settings.ini 的 editor_font_file / ui_font_file / code_font_file）。
// 传空串 = 恢复预设。编辑区字体立刻生效；界面字体还要顺带换掉框架的全局默认字体，
// 否则按钮/对话框/下拉这些不暴露 fontFamily 的组件不会跟着变；
// 代码字体管代码块与行内代码的等宽字面（空 = 系统等宽预设）。
void applyEditorFontFile(AppState& state, std::string path);
void applyUiFontFile(AppState& state, std::string path);
void applyCodeFontFile(AppState& state, std::string path);

// 文件关联（Windows 用户级注册表）：把 EUI-Edits 注册/注销为 .txt/.md 的默认
// 打开方式。系统拒绝直接设默认时也会把应用加进"打开方式"列表，用 toast 说明。
// refreshFileAssocState 查一次注册表回填 state.fileAssocRegistered（设置面板
// 打开时调用），别每帧查。
void registerDefaultFileType(AppState& state);
void unregisterDefaultFileType(AppState& state);
void refreshFileAssocState(AppState& state);

// 恢复界面默认值。只重置界面相关项，不动 vault / last_file。
void resetViewSettings(AppState& state);

} // namespace neo
