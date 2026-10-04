#pragma once

#include "eui_neo.h"

#include "core/window/window_backend.h"
// T16：resetEditorInputState 要在换文档时清 lp 计划缓存（model/lp_decorations.h 的
// invalidatePlanCache）。依赖方向是 state → model（lp 层不 include 本文件，拆层不变）。
#include "model/lp_decorations.h"
// R8：折叠键对账与"命中行的章节祖先链"是纯逻辑，单独一层以便单测。
#include "model/fold_state.h"
#include "model/clean_ai.h"
#include "model/settings.h"
#include "platform/file_assoc.h"
#include "platform/clipboard_image.h"
#include "model/style_schema.h"
#include "model/text_file.h"
#include "model/file_safety.h"
#include "model/vault.h"
#include "model/file_types.h"
#include "model/image_viewport.h"
#include "model/source_decorations.h"
#include "model/tab_presentation.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

// 第一页的兼容入口；应用编辑状态始终通过 editorInputId(document) 按 TabId 隔离。
// 输入组件的光标/撤销栈在 ui.state 中，切页由 syncDocumentTabInputs 交接。
namespace neo {
inline constexpr const char* kEditorInputId = "editor.input.1";
inline constexpr float kMinimumVaultWidth = 180.0f;
inline constexpr float kMaximumVaultWidth = 520.0f;
// 界面缩放的可调范围与默认值。1.0 = 跟随系统（框架已经把系统 DPI 乘进渲染缩放）。
inline constexpr float kMinimumUiScale = 0.80f;
inline constexpr float kMaximumUiScale = 2.00f;
inline constexpr float kDefaultUiScale = 1.00f;
inline constexpr float kUiScaleStep = 0.05f;
// 编辑区字号（逻辑像素）。
inline constexpr float kMinimumEditorFontSize = 12.0f;
inline constexpr float kMaximumEditorFontSize = 32.0f;
inline constexpr float kDefaultEditorFontSize = 16.0f;
// 界面字号（菜单/状态栏/侧栏/设置面板）。上限 18 是实测定下来的：
// 设置面板是手排行高（46 逻辑像素），字号再大标签与说明文字就会互相压。
// 想再往上开，得先把面板改成按字号算行高。
inline constexpr float kMinimumUiFontSize = 12.0f;
inline constexpr float kMaximumUiFontSize = 18.0f;
inline constexpr float kDefaultUiFontSize = 14.0f;
// kUiFontFamily（默认正文字体）随纯样式数据在 model/style_schema.h。

enum class EditorMode { Simple = 0, Vault = 1 };
enum class VaultTab { Files = 0, Outline };
enum class PendingAction { None, NewDocument, OpenFile, OpenPath, ReloadWithEncoding, CloseWindow, CloseTab };
enum class MenuKind { None = 0, File, Edit, View };
// 文档库"新建/重命名"输入弹窗正在办的事（None = 关闭）。
enum class VaultPromptKind { None = 0, NewFile, NewFolder, Rename };
// 设置面板的第二页：None = 正常设置项，其余 = 正在给哪个目标挑字体。
enum class FontPickerTarget { None = 0, Editor, Code, Ui };
enum class SettingsCategory { Appearance = 0, Editor, FileSystem, About };
enum class FindAction { None = 0, Next, Previous, ReplaceOne, ReplaceAll };

struct OutlineEntry {
    std::string title;
    int level = 0;
    int byteOffset = 0;
    int lineNumber = 0;  // 0-based source line
};

struct FindMatch {
    int begin = 0;
    int end = 0;
};

// 菜单栏"编辑"里的命令。菜单回调运行在事件处理阶段（不在 compose 里），
// 那时拿不到 Ui，也就写不了输入组件的内部状态，所以先排进队列，
// 下一次 compose 再执行——和 editorStateDirty 是同一套做法。
// ToggleTask 同理（点复选框的命中回调也在事件阶段），字节偏移随 pendingTaskByte 给。
// ToggleFold / UnfoldAt 要查 lp_plan（本层看不见），applyEditorCommand 直通放行，
// 由 editorView 在 compose 里执行（那里已经有计划缓存）。
// 右键菜单（2026-09-25）的命令：格式/标题/块插入的具体参数放不进枚举，
// 随 AppState 的 pendingFormatKind / pendingHeadingLevel / pendingBlockKind 给。
enum class EditorCommand {
    None = 0, Undo, Redo, Cut, Copy, Paste, SelectAll, ToggleTask, ToggleFold, UnfoldAt,
    FormatInline, SetHeading, InsertBlock, EditLink, CleanAi, InsertImageLink
};

// 全部配色（ThemeMode / EditorColors / rgba / makeColors / markdownStyle /
// 字体纯取值）已拆到 model/style_schema.h（T10）：纯样式数据不依赖 AppState，
// model 层只需 include 那里，state 层继续 include 本文件（下方 editorColors() 等
// 仍按 state().theme 取色）。

struct VaultContext {
    // 文档库
    std::string vaultRoot;
    // 按根共享的只读扫描快照（阶段 B）：不再每页复制整棵树，切页复用同根快照。
    // rows 仍是每页自己的视图投影（per-page expanded/filter），不是共享数据。
    std::shared_ptr<const vault::ScanResult> vaultScan;
    // rows 是从哪个扫描代次投影出来的；与共享快照代次不一致才需要重建。
    std::uint64_t vaultRowsGeneration = 0;
    std::set<std::string> expanded;
    std::vector<vault::Row> rows;
    // 当前文件树行选择；仅用于行焦点/右键反馈，不写入 settings。
    // 相对路径使虚拟列表滚动或重新扫描后仍能识别同一条目。
    std::string vaultSelectedPath;
    std::string vaultRowFocusedPath;
    std::string vaultFocusRestorePath;
    std::string filter;
    float vaultScroll = 0.0f;
    VaultTab vaultTab = VaultTab::Files;
    std::vector<OutlineEntry> outline;
    unsigned long long outlinePlanVersion = static_cast<unsigned long long>(-1);
    float outlineScroll = 0.0f;
    int pendingOutlineJumpByte = -1;
    int outlineCurrentByte = -1;

};

struct DocumentSession : VaultContext {
    std::uint64_t tabId = 1;
    std::unique_ptr<components::input_detail::InputModel::InputState> editorMemory;
    // ── 派生缓存的每页槽位（阶段 C）──────────────────────────────────────────
    // LP 计划 / 装饰表 / 源码高亮三张**全局单例缓存**当前只服务活动页；切页时把
    // 全局缓存 move 进这里、把目标页的 move 回全局，于是每页各自保有自己的计划、
    // 上一代计划、装饰快照与版本号，暖切不再整篇重解析/重排。
    // 它们是可重建的派生缓存：受预算约束、可被淘汰；正文/撤销/光标等编辑状态
    // 仍在 doc 与 editorMemory 里，绝不因此清除。
    std::unique_ptr<lp::PlanCache> derivedPlan;
    std::unique_ptr<lp::DecorationCache> derivedDecoration;
    std::unique_ptr<source::Cache> derivedSource;
    std::uint64_t derivedCacheBytes = 0;  // 估算驻留字节，用于 LRU 预算计账
    std::uint64_t derivedCacheUse = 0;    // LRU 使用时钟
    // 文档
    textfile::Document doc = [] {
        textfile::Document result;
#if defined(_WIN32)
        result.lineEnding = textfile::LineEnding::CrLf;
#endif
        return result;
    }();
    std::string path;                 // 空表示还没落盘的新文档
    unsigned long long revision = 0;
    unsigned long long savedRevision = 0;
    bool recovered = false;
    bool unavailable = false;
    std::string loadError;
    filesafety::Fingerprint diskFingerprint;
    bool suppressRecovery = false;
    std::string recoveredFrom;        // 恢复内容的原始文件路径，仅用于提示
    // 折叠中的标题（S3f 批次 C）：按标题**起始字节偏移** keyed（决策⑦：章节内编辑
    // 会让偏移漂，v1 接受、文档写明）。变化的一侧必须推进 decorationRevision
    // （见 editorView 的折叠处理块），装饰缓存键也吃这个集合（lp 层 foldKey）。
    std::set<int> foldedHeadings;
    // R8：折叠键已对账到的编辑器文本代次（components InputState::textRevision）。
    // 每帧 compose 只比这一个数：相等即零开销，不等才用组件的可信 delta 平移折叠键，
    // 证明不了就清空并展开。换文档时由 resetEditorInputState 拉到新代次。
    unsigned long long foldsTextRevision = 0;
    // R8：查找命中落在折叠章节里时，plan 还没就绪就先把命中偏移排在这里，
    // 等当前 plan 可用的那一帧再展开 —— 不抢先做无 delta 的全量解析。
    int pendingFindRevealByte = -1;

    // Current document literal search. Matches are rebuilt only when the query
    // or document revision changes, never on an idle frame.
    bool findOpen = false;
    bool findFocusPending = false;
    bool findEditorFocusPending = false;
    bool findPrefillSelection = false;
    bool findReplaceOpen = false;
    bool findOptionsOpen = false;
    bool findMatchCase = false;
    bool findWholeWord = false;
    bool findWrap = true;
    bool findMatchesCase = false;
    bool findMatchesWholeWord = false;
    std::string findNotice;
    std::string findQuery;
    std::string findReplacement;
    std::vector<FindMatch> findMatches;
    int findCurrent = -1;
    unsigned long long findMatchesRevision = static_cast<unsigned long long>(-1);
    std::string findMatchesQuery;
    FindAction pendingFindAction = FindAction::None;
    // 地址栏（侧栏顶部那一行）的文本。它跟 vaultRoot 同步，但必须单独存一份：
    // 输入组件的 value 是受控的，每帧从 vaultRoot 现算会把用户正在敲的内容冲掉。
    std::string vaultAddress;
    // 跳转后要把目标行滚进视野，而行号要等 rows 重建完才存在，
    // 所以跳转那帧只记相对路径，由 vault_view 下一帧消费。
    std::string vaultPendingReveal;
    // 状态栏统计：按 revision 缓存，避免每次 compose 都扫全文档
    unsigned long long statsRevision = static_cast<unsigned long long>(-1);
    textfile::Stats stats;

    // 新建/打开文件后需要重建输入组件内部状态
    bool editorStateDirty = false;

    std::chrono::steady_clock::time_point lastRecoveryWrite{};
    // 节流窗口内又发生过编辑：到期后由 tickRecoveryWrite 补写一次（自动到期），
    // 避免"最后一次编辑落在节流窗口里、之后没有帧就一直不落盘"。
    bool recoveryPendingWrite = false;
    bool dirty() const { return revision != savedRevision; }
    std::string displayName() const {
        return path.empty() ? std::string(i18n::tr("safety.unsaved_name")) : textfile::fileName(path);
    }

    std::string newDocumentLanguage = "text";
    std::string languageOverride;
    int wrapOverride = -1;
    std::string language() const { return languageOverride.empty() ? (path.empty() ? newDocumentLanguage : filetypes::detect(path).language) : languageOverride; }
    bool markdownCapable() const { return language() == "markdown"; }
    bool sourceView() const { return filetypes::source(language()); }
    bool wordWrap() const { return wrapOverride < 0 ? !sourceView() : wrapOverride != 0; }
};

struct AppState : DocumentSession {
    std::unordered_map<std::uint64_t, DocumentSession> inactiveTabs;
    std::vector<std::uint64_t> tabOrder{1};
    std::uint64_t nextTabId = 2;
    std::uint64_t displayedInputTabId = 0;
    std::vector<std::uint64_t> releasedTabIds;
    // 上次 mergeRelatedVaultRoots 使用的"打开根集合"签名；集合不变时换页可短路这次 O(n²) 规范化。
    std::vector<std::string> mergedVaultRootSignature;
    std::uint64_t confirmationTabId = 0;
    bool exitClosing = false;
    bool sessionClosePending = false; // confirmed exit: asynchronous record cleanup, no more edits
    bool sessionStorageBlocked = false;
    bool recoveryWriteWarning = false;
    bool pendingNewMarkdown = false;
    float tabScroll = 0.0f;
    bool tabListOpen = false;
    std::uint64_t tabListReveal = 0;

    // ── 标签展示缓存（非持久化；2026-10-03 视觉改进批次）─────────────────────
    // 由 refreshTabPresentation 在"根文本变化的动作阶段"统一更新；绘制只读这里的结果，
    // 绝不在每帧绘制/列表/提示路径里规范化根或查询文件系统。颜色只是展示信息，
    // 不参与文件定位、库扫描或恢复身份判断。退出即丢弃，不写设置、不进会话清单。
    tabpresentation::Registry tabColorRegistry;
    std::unordered_map<std::uint64_t, std::string> tabPresentationKeyByTab;   // 上一轮 TabId→canonical key（供合并继承）
    std::unordered_map<std::uint64_t, int> tabPresentationSlot;               // 当前 TabId→色槽
    std::unordered_map<std::uint64_t, std::uint64_t> tabPresentationOrdinal;  // 当前 TabId→组序号
    std::unordered_map<std::string, std::string> tabCanonicalRootCache;       // 原始根文本→canonical key
    std::vector<std::string> tabPresentationSignature;                        // 上次刷新的轻量签名

    // ── 顶栏固定指针连续关闭的会话临时锚点（不持久化、无磁盘写、无历史记录）──
    struct CloseAnchor {
        bool enabled = false;
        float anchorX = 0.0f;              // 关闭按钮中心在区域内的局部 X（DIP）
        float anchorY = 0.0f;
        std::uint64_t closedTabId = 0;
        std::uint64_t expectedNextId = 0;
        std::vector<std::uint64_t> beforeTabOrder;
        float drawOffset = 0.0f;           // 仅绘制偏移；不写入 tabScroll
    };
    CloseAnchor closeAnchor;
    // compose 里用于判定"锚点是否成立"的中间态：本次关闭链尚未应用（等待下一次 compose 验证）。
    bool closeAnchorPendingApply = false;
    bool closeAnchorRevealPending = false;

    filesafety::Fingerprint conflictFingerprint;
    std::string conflictPath;
    bool saveConflictOpen = false;
    int safetyChoice = 2;
    bool safetyDialogWasOpen = false;
    float safetyScroll = 0;
    bool continueAfterSave = false;
    bool closeApproved = false;
    bool savePickerOpen = false;      // native modal helper; preserves document transaction during nested paints

    // 视图
    EditorMode mode = EditorMode::Simple;
    float vaultWidth = 264.0f;

    // 界面缩放与字号。uiScale = 1.0 表示跟随系统（框架已乘系统 DPI），
    // 调大是在系统缩放之上额外放大。两者都落盘到 settings.ini。
    float uiScale = 1.0f;
    float editorFontSize = 16.0f;
    // 界面字号。菜单/状态栏/侧栏/设置面板都从它派生（见 ui/metrics.h）。
    float uiFontSize = kDefaultUiFontSize;
    // 自选字体文件（空 = 用上面的预设名）。界面文字走 uiFontFile，
    // 编辑区与预览走 editorFontFile，代码块/行内代码走 codeFontFile（等宽）。
    std::string editorFontFile;
    std::string uiFontFile;
    std::string codeFontFile;
    // 界面主题。落盘到 settings.ini 的 theme。
    ThemeMode theme = ThemeMode::Dark;
    // 主题偏好 = "跟随系统"（settings.ini 的 theme=2）：theme 字段此时存的是
    // 启动/点击时解析出的实际模式，每次都按系统 AppsUseLightTheme 重新解析。
    bool themeFollowSystem = false;
    // 当前生效的主题文件（UTF-8 路径，空 = 内置配色）。落盘到 last_theme_file，
    // 设置面板的"主题文件"行按它显示；配色数据本体在 model 层的 activeTheme()，
    // 两边由 load/reset 一起改（见 ui/settings_panel.h 的主题文件入口）。
    std::string themeFile;
    bool themeListOpen = false;
    float themeListScroll = 0.0f;
    bool showStatusBar = true;
    // 编辑区行号列。落盘到 settings.ini 的 line_numbers。
    bool showLineNumbers = true;
    // 限制行宽（Obsidian 的"可读行宽"）：内容列收窄到 43.75em（700px @ 16）并居中，
    // 左右留白换阅读性。落盘到 settings.ini 的 readable_width。默认开（与 Obsidian 一致）。
    bool readableWidth = true;
    // 界面动画（悬停/按压配色过渡、按压缩放等反馈动画）。落盘到 settings.ini 的
    // animations；首启值取自系统"动画效果"设置。关闭后界面即时响应、不产生过渡帧。
    bool animations = true;
    // 启动时问系统要的屏幕缩放值，只用于显示说明和推导窗口物理尺寸。
    float systemScale = 1.0f;

    // 菜单栏与设置面板的开合状态
    MenuKind openMenu = MenuKind::None;
    bool settingsOpen = false;
    SettingsCategory settingsCategory = SettingsCategory::Appearance;
    float settingsScroll = 0.0f;
    // 上一帧的 settingsOpen：面板刚打开的那一帧要查一次文件关联状态
    // （注册表 IO 不进每帧路径）。设置面板用 fileAssocRegistered 画开关挡位。
    bool settingsOpenLast = false;
    bool fileAssocRegistered = false;
    bool fileAssocEntriesPresent = false;
    fileassoc::DefaultStatus fileAssocDefaults;
    // 设置面板的字体选择页（占满整个面板内容区，不叠第二层 dialog：
    // 框架的命中顺序不按 zIndex 走，叠层是踩过的坑）。
    FontPickerTarget fontPicker = FontPickerTarget::None;
    float fontListScroll = 0.0f;
    EditorCommand pendingEditorCommand = EditorCommand::None;
    // ToggleTask 的载荷：要翻转的任务状态字符偏移（-1 = 无）。
    // 命中判定发生在按下那一刻（事件阶段），真正改字节在 compose（同上，见 EditorCommand）。
    int pendingTaskByte = -1;
    // 编辑区右键菜单（2026-09-25）：open 时在 (contextMenuX/Y)（窗口逻辑坐标）
    // 显示。菜单本体在 editorContextMenuOverlay（app.cpp 里与其它弹层一起最后画）。
    bool contextMenuOpen = false;
    float contextMenuX = 0.0f;
    float contextMenuY = 0.0f;
    // 右键菜单命令的载荷（事件阶段排队、compose 执行，同 pendingTaskByte）。
    // FormatInline：1粗体 2倾斜 3删除线 4高亮 5行内代码 6数学 7注释 8清除格式。
    int pendingFormatKind = 0;
    // SetHeading：0=正文（移除标题标记），1..6 = 标题级别。
    int pendingHeadingLevel = 0;
    // InsertBlock：1代码块 2表格 3引用 4分割线 5内部链接 6外部链接。
    int pendingBlockKind = 0;
    // EditLink 的载荷（右键菜单"编辑链接"）：要替换的 URL 字节区间与新值。
    // 区间在菜单里点"编辑链接"时记下，弹窗确认后才消费。
    int pendingLinkBeg = -1;    int pendingLinkEnd = -1;
    std::string pendingLinkUrl;
    // 图片粘贴（R2）：pendingImagePaste 在事件/compose 阶段置位（统一 Ctrl+V 与
    // 右键"粘贴"的图片分支），每帧 tick 消费——剪贴板读取、PNG 编码、附件落盘都
    // 在那一步做完，只把最终 Markdown 链接放进 pendingImageLink，经
    // EditorCommand::InsertImageLink 在 compose 里落到光标处（撤销一步）。
    bool pendingImagePaste = false;
    std::string pendingImageLink;
    // "编辑链接"弹窗（文本输入在 components::input 里，值回写 linkEditorUrl）。
    bool linkEditorOpen = false;
    std::string linkEditorUrl;
    // 全屏图片预览（S3f 批次 E，批次 B 的收尾增量）：路径非空 = 预览打开。
    // 宽高是**原图**的（打开时按文件头读一次），预览按窗口 contain 重算缩放，
    // 与编辑区里那颗缩略图的尺寸无关。
    std::string imagePreviewPath;
    float imagePreviewWidth = 0.0f;
    float imagePreviewHeight = 0.0f;
    ImageViewport imageViewport;

    // 文档库右键菜单（2026-09-26）：open 时在 (vaultContextMenuX/Y) 显示，
    // 操作目标（库内相对路径，'/' 分隔）与类型随 vaultContextPath/IsDir 给。
    bool vaultContextMenuOpen = false;
    float vaultContextMenuX = 0.0f;
    float vaultContextMenuY = 0.0f;
    std::string vaultContextPath;
    bool vaultContextIsDir = false;
    // 文档库"新建/重命名"输入弹窗与"删除"确认弹窗。
    VaultPromptKind vaultPromptKind = VaultPromptKind::None;
    std::string vaultPromptText;
    std::string vaultPromptError;
    std::string vaultRenameSuggestedName;
    std::string vaultRenameExtensionApprovalName;
    bool vaultPromptFocusPending = false;
    bool vaultRenameReturnToRow = false;
    float vaultPromptScroll = 0.0f;
    bool vaultDeletePending = false;

    PendingAction pending = PendingAction::None;
    std::string pendingPath;          // PendingAction::OpenPath 的目标文件
    // PendingAction::ReloadWithEncoding 的载荷：确认丢弃修改后按此编码重开。
    textfile::ForcedEncoding pendingEncoding;

    bool toastVisible = false;
    std::string toastTitle;
    std::string toastMessage;



    enum class RiskAction { None, Confirm };
    RiskAction riskAction = RiskAction::None;
    std::string riskTitle, riskMessage, riskConfirm;
    std::function<void()> riskCallback;
    bool languageMenuOpen = false;
    std::string associationSearch;
    int associationCategory = 0;
    bool associationSelectionLoaded = false;
    std::vector<std::string> associationSelected;
    std::vector<fileassoc::TypeStatus> associationStatuses;
};

inline std::string editorInputId(std::uint64_t id) { return std::string("editor.input.") + std::to_string(id); }
inline std::string editorInputId(const DocumentSession& document) { return editorInputId(document.tabId); }
// Shared persistence funnel; implementation keeps all dirty tabs in independent recovery files.
bool persistDocumentSession(AppState& state);
// 退出屏障：提交最终会话并等待 commit ack（有界超时，只在关闭/退出路径用）。
bool flushDocumentSession(AppState& state, int timeoutMs = 5000);
void syncDocumentTabInputs(eui::Ui& ui, AppState& state);
inline auto& sessionRecoveryWriter() {
    static bool (*writer)(AppState&) = nullptr;
    return writer;
}

inline void requestRisk(AppState& state, std::string title, std::string message, std::string confirm, std::function<void()> callback) {
    state.riskTitle=std::move(title);state.riskMessage=std::move(message);state.riskConfirm=std::move(confirm);
    state.riskCallback=std::move(callback);state.riskAction=AppState::RiskAction::Confirm;
    app::requestUpdate();
}
inline void cancelRisk(AppState& state) {state.riskAction=AppState::RiskAction::None;state.riskCallback={};}
inline void confirmRisk(AppState& state) {auto action=std::move(state.riskCallback);cancelRisk(state);if(action) action();}

inline AppState& state() {
    static AppState instance;
    return instance;
}

// 当前主题的配色（按 state().theme）。editorColors(ThemeMode) 的纯定义在
// model/style_schema.h —— 本文件只保留这个查 state() 的便利版。
inline const EditorColors& editorColors() {
    return editorColors(state().theme);
}

// Markdown 预览 / Live Preview 主题：纯版 markdownStyle(..., colors) 在
// model/style_schema.h（配色作参数传入）。这里保留查 state().theme 的便利包装，
// 旧调用点（单测等）语义不变。
inline components::MarkdownStyle markdownStyle(float bodySize, const char* bodyFontFamily,
                                               const char* codeFontFamily) {
    return markdownStyle(bodySize, bodyFontFamily, codeFontFamily, editorColors(), state().theme);
}

// 界面字体：自选文件优先，否则用预设名。
//
// 为什么必须有这个函数：界面上的每一处 ui.text(...) 都显式传了 fontFamily，
// 而它原来的值是常量 kUiFontFamily —— 所以"改设置里的界面字体"要想一次改全，
// 就得让所有调用点都从这里取。按钮/对话框/下拉这些不暴露 fontFamily 的组件
// 走框架的全局默认字体，由 applyUiFontFile() 调 setDefaultFontFiles() 一起换掉。
inline const char* uiFontFamily(const AppState& state) {
    return state.uiFontFile.empty() ? kUiFontFamily : state.uiFontFile.c_str();
}

// 编辑区与预览的字体 / 代码字体：纯取值（接 fontFile 字符串）在
// model/style_schema.h，这里只是带 AppState 的便利包装。
// 代码块/行内代码另有独立的代码字体（见 codeFontFamily），不受这里影响。
inline const char* editorFontFamily(const AppState& state) {
    return editorFontFamily(state.editorFontFile);
}

// 代码字体：自选了文件就用它，否则用"monospace"预设 —— 框架会把这个名字解析成
// 系统等宽字体（Windows 上 Cascadia Mono / Consolas，见 core/render/text.cpp）。
// 正文预设是比例字体，代码块用它缩进/对齐全乱，所以等宽是代码块的默认而非可选项。
inline const char* codeFontFamily(const AppState& state) {
    return codeFontFamily(state.codeFontFile);
}

// ── R8：折叠状态的唯一更新入口 ────────────────────────────────────────────────
// 折叠键是字节偏移：文本一变就可能错位，换文档更是直接指向别的行。以前只有 CleanAi
// 成功路径清空，打字 / 粘贴 / 剪切 / 图片链接插入 / 命令回写 / undo-redo / 换文档全都没管。
//
// 每帧 compose 调一次（editorView 开头、input build() 之前），只比一个代次：
//   · 代次相等 → 直接返回（零开销）；
//   · 代次只差 1 且组件给了可信 delta → 平移折叠键（键落在被替换区间内部则失败）；
//   · 其余（一帧跨多次编辑 / 无 delta / 外部赋值）→ 清空并展开。
// 宁可多展开也不能留着错位的键：错位的键折叠的是**别的章节**。
inline void syncFoldsWithEditorText(AppState& appState,
                                    components::input_detail::InputModel::InputState& input) {
    if (appState.foldsTextRevision == input.textRevision) {
        return;
    }
    // 文本动过了：排队中的"延迟展开"按字节偏移存，偏移可能已经漂走 —— 一并作废，
    // 由下一次查找动作重新算。
    appState.pendingFindRevealByte = -1;
    const components::input_detail::PendingTextEdit& edit = input.pendingEdit;
    const bool chainIntact = edit.valid && edit.revision == input.textRevision &&
                             appState.foldsTextRevision + 1 == input.textRevision;
    std::set<int> mapped = appState.foldedHeadings;
    bool ok = mapped.empty();
    if (!ok && chainIntact) {
        ok = foldstate::mapFoldedKeys(mapped, edit);
    }
    if (!ok) {
        mapped.clear();
    }
    appState.foldsTextRevision = input.textRevision;
    if (mapped != appState.foldedHeadings) {
        appState.foldedHeadings = std::move(mapped);
        // 折叠键变了必须推进 decorationRevision：根 dirtyKey 含它，整棵子树才会按
        // 新装饰重建（同 ToggleFold 的做法）。只改集合不推，等于继续画旧折叠态。
        ++input.decorationRevision;
    }
}

// 展开"命中偏移所在章节"的全部祖先标题（由内到外，见 foldstate::sectionAncestorKeys）。
// plan 未就绪（缓存无效或文本代次不符）时不抢先做无 delta 解析：把偏移排进
// pendingFindRevealByte 并返回 false，由每帧的 applyPendingFindReveal 在 plan 就绪后重试。
// 返回 true = 已处理（可能本来就无需展开）。
inline bool revealFoldsAtByte(AppState& appState,
                              components::input_detail::InputModel::InputState& input,
                              int byteOffset) {
    const lp::PlanCache& cache = lp::planCache();
    if (!cache.valid || cache.text != appState.doc.text) {
        appState.pendingFindRevealByte = byteOffset;
        return false;
    }
    bool changed = false;
    for (const int key : foldstate::sectionAncestorKeys(cache.plan, byteOffset)) {
        changed = appState.foldedHeadings.erase(key) > 0 || changed;
    }
    appState.pendingFindRevealByte = -1;
    if (changed) {
        ++input.decorationRevision;
    }
    return true;
}

// 每帧消费延迟展开。返回 true = plan 还没就绪、需要下一帧重试（调用方 requestUpdate，
// 后台唤醒规则：不 requestUpdate 的话按需渲染不会再来一帧）。
inline bool applyPendingFindReveal(AppState& appState,
                                   components::input_detail::InputModel::InputState& input) {
    if (appState.pendingFindRevealByte < 0) {
        return false;
    }
    return !revealFoldsAtByte(appState, input, appState.pendingFindRevealByte);
}

// 换文档时清折叠态（R8，指引 §4 R8 缺口 2）。折叠键是按字节偏移 keyed 的，换一篇
// 文档后那些偏移指向的是别的行 —— 甚至可能"碰巧相同"而折叠出莫名其妙的章节。
// 同时把对账代次对齐到新文档，避免下一帧被误判成"文本变过"而再动一次折叠集合。
// 单独成函数是为了让这条规则可以被单测直接钉住（resetEditorInputState 需要 Ui）。
inline void clearFoldsForDocumentSwitch(AppState& appState,
                                        components::input_detail::InputModel::InputState& input) {
    appState.foldedHeadings.clear();
    appState.pendingFindRevealByte = -1;
    appState.foldsTextRevision = input.textRevision;
}

// 把文档内容直接写进输入组件的内部状态：光标回到文首、滚动归零、撤销栈清空。
// 输入组件在 state.text 与传入 value 不一致时会重置光标到末尾，这里先对齐两者。
//
// 字段逐一复现的实现在组件侧（InputModel::loadDocument，**与 neo::loadDocument
// 磁盘 API 同名、不同命名空间**）。这里只负责应用层的取用：额外的 lp 装饰计划缓存、
// foldedHeadings 等仍然留在应用层，组件不认识它们。
inline void resetEditorInputState(eui::Ui& ui, const std::string& text) {
    using InputModel = components::input_detail::InputModel;
    auto& inputState = ui.state<InputModel::InputState>(editorInputId(state()));
    InputModel::loadDocument(inputState, text);
    // T16：换成**非 markdown** 文档时，装饰层不会再调 cachedPlan（editor_view 的
    // decorationsForEditor 对非 md 直接早退），上一篇 markdown 的 text/plan 会一直滞留
    // 在 lp 计划缓存里 —— 就在这个换文档的唯一入口把它清掉：内容（valid/text/plan）
    // 清空，version 单调**不归零**（归零会让老装饰表的 planVersion 键重新命中，拿上一篇
    // 文档的表画这一篇；详见 lp::invalidatePlanCache 的注释）。
    // markdown 文档不清：计划按文本自证，下一次 cachedPlan 自然换代。
    // 这一步必须留在应用层：组件侧的 loadDocument 不认识 lp（禁止从 components
    // include app model），所以它只搬它自己的字段。
    if (!state().markdownCapable()) {
        source::invalidate();
    lp::invalidatePlanCache();
    }
    // R8：换文档一律清折叠态（指引 §4 R8 缺口 2）。
    clearFoldsForDocumentSwitch(state(), inputState);
}

// 应急副本记录"来源路径"：已恢复过的文稿 path 为空，但来源文件名值得留到下一次关闭，
// 这样状态栏的"来自 X"提示不会在反复重启后消失。
inline std::string recoveryOrigin(const AppState& appState) {
    return appState.path.empty() ? appState.recoveredFrom : appState.path;
}

// 未保存内容定期落盘，最多每 1.5 秒写一次；保存成功后清除。
// 写入现在走串行后台协调器（阶段 E），这里只负责节流与"自动到期"：
//   · 窗口内重复调用 → 只置 recoveryPendingWrite，等 tickRecoveryWrite 到期补写；
//   · 到期后调用 → 立即提交（提交的是拥有的不可变快照，不是 UI 线程上的磁盘事务）。
inline void maybeWriteRecovery(AppState& appState) {
    if (appState.suppressRecovery) return;
    const auto now = std::chrono::steady_clock::now();
    if (appState.lastRecoveryWrite.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - appState.lastRecoveryWrite).count() < 1500) {
        appState.recoveryPendingWrite = true;  // 自动到期：不无限推迟这次修改
        return;
    }
    appState.lastRecoveryWrite = now;
    appState.recoveryPendingWrite = false;
    if (sessionRecoveryWriter()) sessionRecoveryWriter()(appState);
    else settings::writeRecovery(appState.doc.text, recoveryOrigin(appState), &appState.doc);
}

// 每帧调用：节流窗口到期且还有待写修改时补提交一次。没有待写就不做任何事。
inline void tickRecoveryWrite(AppState& appState) {
    if (!appState.recoveryPendingWrite || appState.suppressRecovery) return;
    const auto now = std::chrono::steady_clock::now();
    if (appState.lastRecoveryWrite.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - appState.lastRecoveryWrite).count() < 1500) {
        return;
    }
    appState.lastRecoveryWrite = now;
    appState.recoveryPendingWrite = false;
    if (sessionRecoveryWriter()) sessionRecoveryWriter()(appState);
}

// 点任务复选框翻转勾选（S3f 批次 A）：pos 必须正指着 [ ]/[x] 的状态字符——
// 从按下到执行隔了一帧，文本可能已经变了，形状不符就整个放弃，绝不去改形状不明的字节。
inline bool flipTaskCheckboxAt(components::input_detail::InputModel::InputState& input, int pos) {
    using InputModel = components::input_detail::InputModel;
    if (pos < 1 || pos + 1 >= static_cast<int>(input.text.size())) {
        return false;
    }
    const std::size_t p = static_cast<std::size_t>(pos);
    const char state = input.text[p];
    if (input.text[p - 1] != '[' || input.text[p + 1] != ']' ||
        (state != ' ' && state != 'x' && state != 'X')) {
        return false;
    }
    // 单字节改动也走增量捕获：span 就是状态字符那一个字节（pos 是 int 边界）。
    InputModel::beginEdit(input, pos, pos + 1);
    input.text[p] = state == ' ' ? 'x' : ' ';
    InputModel::endEdit(input);
    return true;
}

// ── 右键菜单的文本命令（2026-09-25）──────────────────────────────────
// 全部只在 compose 里跑（applyEditorCommand）：命令前 beginEdit 捕获一个**连续 before
// span**、命令后 endEdit 收缩提交（记录入栈 + bump textRevision 都在 endEdit 里做）。
// applyEditorCommand 的公共尾巴只回写 doc.text，不再自己 bump —— 否则一次改动会 bump 两次。

inline int orderedSelectionBegin(const components::input_detail::InputModel::InputState& input) {
    return std::min(input.selectionStart, input.selectionEnd);
}

inline int orderedSelectionEnd(const components::input_detail::InputModel::InputState& input) {
    return std::max(input.selectionStart, input.selectionEnd);
}

// 用成对标记包住选区：标记已紧贴选区两侧时解开，否则包上。无选区时插入一对
// 空标记、光标落中间（Obsidian 的行为）。选区保持包住内侧文字，连续点同一种
// 格式即"开关"。
inline bool applyInlineFormat(components::input_detail::InputModel::InputState& input,
                              const char* marker) {
    using InputModel = components::input_detail::InputModel;
    const std::string m = marker;
    const std::size_t size = m.size();
    const int beg = orderedSelectionBegin(input);
    const int end = orderedSelectionEnd(input);
    const bool wrapped =
        beg >= static_cast<int>(size) &&
        end + static_cast<int>(size) <= static_cast<int>(input.text.size()) &&
        input.text.compare(static_cast<std::size_t>(beg - size), size, m) == 0 &&
        input.text.compare(static_cast<std::size_t>(end), size, m) == 0;
    // 这里是**两点修改**（解开时要各吃掉一处标记），不强行压成单点：把两点之间的
    // 整个连续区间当作一个 span 的 before/after 捕获，endEdit 收缩后得到语义等价的
    // 最小变换 —— 一次撤销正好回到命令前的文本。
    if (wrapped) {
        InputModel::beginEdit(input, beg - static_cast<int>(size), end + static_cast<int>(size));
        input.text.erase(static_cast<std::size_t>(end), size);
        input.text.erase(static_cast<std::size_t>(beg - size), size);
        input.selectionStart = beg - static_cast<int>(size);
        input.selectionEnd = end - static_cast<int>(size);
    } else {
        InputModel::beginEdit(input, beg, end);
        input.text.insert(static_cast<std::size_t>(end), m);
        input.text.insert(static_cast<std::size_t>(beg), m);
        if (end > beg) {
            input.selectionStart = beg + static_cast<int>(size);
            input.selectionEnd = end + static_cast<int>(size);
        } else {
            input.selectionStart = beg + static_cast<int>(size);
            input.selectionEnd = input.selectionStart;
        }
    }
    input.cursor = input.selectionEnd;
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// [文本](目标) / ![说明](目标) → 文本/说明。返回剥完的结果（无匹配则原样）。
inline std::string stripMarkdownLinks(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '[') {
            const std::size_t close = text.find(']', i);
            if (close != std::string::npos && close + 1 < text.size() && text[close + 1] == '(') {
                const std::size_t paren = text.find(')', close + 2);
                if (paren != std::string::npos) {
                    out += text.substr(i + 1, close - i - 1);
                    i = paren + 1;
                    continue;
                }
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

// 清除格式：选区里的链接还原成纯文本，再反复剥首尾的成对标记。
// 只动选区（无选区 = 不动），嵌套标记靠"剥到不动为止"收敛。
inline bool clearInlineFormat(components::input_detail::InputModel::InputState& input) {
    using InputModel = components::input_detail::InputModel;
    const int beg = orderedSelectionBegin(input);
    const int end = orderedSelectionEnd(input);
    if (beg == end) {
        return false;
    }
    std::string result = stripMarkdownLinks(
        input.text.substr(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg)));
    static constexpr const char* kMarkers[] = {"**", "__", "==", "%%", "~~", "*", "_", "`", "$"};
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const char* marker : kMarkers) {
            const std::string m = marker;
            bool pairStripped = true;
            while (pairStripped && result.size() >= m.size() * 2 &&
                   result.compare(0, m.size(), m) == 0 &&
                   result.compare(result.size() - m.size(), m.size(), m) == 0) {
                result.erase(result.size() - m.size(), m.size());
                result.erase(0, m.size());
                pairStripped = false;
                stripped = true;
            }
        }
    }
    if (result.empty() || result == input.text.substr(static_cast<std::size_t>(beg),
                                                      static_cast<std::size_t>(end - beg))) {
        return false;
    }
    InputModel::beginEdit(input, beg, end);
    input.text.replace(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg), result);
    input.selectionStart = beg;
    input.selectionEnd = beg + static_cast<int>(result.size());
    input.cursor = input.selectionEnd;
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// 选区转链接：有选区 → [选区](target)，光标落 target 内；无选区 → 插入
// [文本](target) 并选中"文本"三个字（Obsidian 的做法，提示用户替换）。
inline bool applyLinkFormat(components::input_detail::InputModel::InputState& input,
                            const char* target) {
    using InputModel = components::input_detail::InputModel;
    const int beg = orderedSelectionBegin(input);
    const int end = orderedSelectionEnd(input);
    const std::string suffix = std::string("](") + target + ")";
    // span 覆盖两种分支的全部改动：有选区 = 替换 [beg,end)，无选区 = 在 beg 处插入。
    InputModel::beginEdit(input, beg, end);
    if (end > beg) {
        const std::string selected =
            input.text.substr(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg));
        input.text.replace(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg),
                           "[" + selected + suffix);
        input.selectionStart = beg + 1;
        input.selectionEnd = beg + 1 + static_cast<int>(selected.size());
        input.cursor = end + 2;  // 目标占位串的开头
    } else {
        input.text.insert(static_cast<std::size_t>(beg), i18n::tr("safety.link_text") + suffix);
        input.selectionStart = beg + 1;
        input.selectionEnd = beg + static_cast<int>(std::string(i18n::tr("safety.link_text")).size());
        input.cursor = input.selectionEnd;
    }
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// 右键菜单"编辑链接"（2026-09-26）：把链接 span 里的 URL 部分（[urlBeg, urlEnd)，
// 由 model/link_target.h 的 linkTargetAt 算出）替换成新值，链接文字原样保留。
// 撤销历史按一次编辑记录（beginEdit/endEdit）。
inline bool applyEditLink(components::input_detail::InputModel::InputState& input,
                          int urlBeg, int urlEnd, const std::string& url) {
    using InputModel = components::input_detail::InputModel;
    if (urlBeg < 0 || urlEnd < urlBeg || urlEnd > static_cast<int>(input.text.size())) {
        return false;
    }
    const int begin = InputModel::clampUtf8Boundary(input.text, urlBeg);
    const int end = InputModel::clampUtf8Boundary(input.text, urlEnd);
    InputModel::beginEdit(input, begin, end);
    input.text.replace(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin), url);
    input.cursor = begin + static_cast<int>(url.size());
    input.selectionStart = input.cursor;
    input.selectionEnd = input.cursor;
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// 选区覆盖的每一行做"行首前缀"编辑：标题（含旧标记清理）与引用共用。
// edits 先收集再从后往前套用，选区偏移按每行前缀差精确补偿。
inline bool applyLinePrefix(components::input_detail::InputModel::InputState& input,
                            const std::string& prefix,
                            bool stripHeading) {
    using InputModel = components::input_detail::InputModel;
    const std::string& t = input.text;
    int lineBeg = orderedSelectionBegin(input);
    while (lineBeg > 0 && t[static_cast<std::size_t>(lineBeg - 1)] != '\n') {
        --lineBeg;
    }
    int lineEnd = orderedSelectionEnd(input);
    while (lineEnd < static_cast<int>(t.size()) && t[static_cast<std::size_t>(lineEnd)] != '\n') {
        ++lineEnd;
    }
    struct LineEdit {
        int start;
        int end;
        std::string replacement;
    };
    std::vector<LineEdit> edits;
    for (int pos = lineBeg; pos <= lineEnd;) {
        int lineStop = pos;
        while (lineStop < static_cast<int>(t.size()) && t[static_cast<std::size_t>(lineStop)] != '\n') {
            ++lineStop;
        }
        int contentBeg = pos;
        int spaces = 0;
        while (contentBeg < lineStop && t[static_cast<std::size_t>(contentBeg)] == ' ' && spaces < 3) {
            ++contentBeg;
            ++spaces;
        }
        int afterHash = contentBeg;
        int hashes = 0;
        while (afterHash < lineStop && t[static_cast<std::size_t>(afterHash)] == '#' && hashes <= 6) {
            ++afterHash;
            ++hashes;
        }
        const bool hadHeading = stripHeading && hashes >= 1 && hashes <= 6 &&
                                (afterHash == lineStop || t[static_cast<std::size_t>(afterHash)] == ' ');
        int oldEnd = pos;
        if (hadHeading) {
            oldEnd = afterHash == lineStop ? afterHash : afterHash + 1;  // 吃掉标题后的一个空格
        }
        if (hadHeading || !prefix.empty()) {
            // 已是目标级别 → 空替换 = 关掉（对同级别标题再点一次即取消）。
            edits.push_back({pos, oldEnd,
                             hadHeading && prefix.size() == static_cast<std::size_t>(hashes) + 1
                                 ? std::string{}
                                 : prefix});
        }
        if (lineStop == static_cast<int>(t.size())) {
            break;
        }
        pos = lineStop + 1;
    }
    if (edits.empty()) {
        return false;
    }
    // 幂等：所有行都已是目标前缀（比如对已是 "## " 的行再设 2 级）就不动文档。
    const bool allNoop = std::all_of(edits.begin(), edits.end(), [](const LineEdit& edit) {
        return edit.replacement.empty() && edit.start == edit.end;
    });
    if (allNoop) {
        return false;
    }
    // 整段行首前缀是**多个分散编辑**：按"连续 before span"捕获（行首到行尾整段），
    // 命令跑完由 endEdit 收缩成一条记录 —— 撤销一步回到命令前，而不是逐行回退。
    InputModel::beginEdit(input, lineBeg, lineEnd);
    int selectionBeg = orderedSelectionBegin(input);
    int selectionEnd = orderedSelectionEnd(input);
    for (std::size_t index = edits.size(); index-- > 0;) {
        const LineEdit& edit = edits[index];
        const int oldLength = edit.end - edit.start;
        const int delta = static_cast<int>(edit.replacement.size()) - oldLength;
        input.text.replace(static_cast<std::size_t>(edit.start), static_cast<std::size_t>(oldLength),
                           edit.replacement);
        // 选区补偿：行首之前的偏移不归这行管；落在旧前缀里的夹逼到新前缀长度。
        const auto adjust = [&](int& offset) {
            if (offset >= edit.end) {
                offset += delta;
            } else if (offset > edit.start) {
                offset = edit.start + std::min(offset - edit.start,
                                               static_cast<int>(edit.replacement.size()));
            }
        };
        adjust(selectionBeg);
        adjust(selectionEnd);
    }
    input.selectionStart = InputModel::clampUtf8Boundary(input.text, selectionBeg);
    input.selectionEnd = InputModel::clampUtf8Boundary(input.text, selectionEnd);
    input.cursor = input.selectionEnd;
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// 块级插入（右键菜单"插入"子菜单）。代码块/表格/分割线插模板并把光标落到
// 要紧接输入的位置；引用给当前行（或选区各行）加 "> " 前缀。
inline bool insertBlockTemplate(components::input_detail::InputModel::InputState& input, int kind) {
    using InputModel = components::input_detail::InputModel;
    if (kind == 3) {
        return applyLinePrefix(input, "> ", false);
    }
    const int cursor = input.cursor;
    const bool atLineStart = cursor == 0 || input.text[static_cast<std::size_t>(cursor - 1)] == '\n';
    std::string snippet;
    int landing = 0;
    if (kind == 1) {  // 代码块：光标落到首行代码处
        snippet = atLineStart ? "```\n\n```\n" : "\n```\n\n```\n";
        landing = static_cast<int>(snippet.find('\n')) + 1;
    } else if (kind == 2) {  // 表格：光标落到数据行第一格
        snippet = atLineStart ? i18n::tr("safety.table_template")
                              : i18n::tr("safety.table_template_newline");
        const std::size_t dataRow = snippet.rfind("\n|  |");
        landing = static_cast<int>(dataRow) + 3;
    } else if (kind == 4) {  // 分割线
        snippet = atLineStart ? "---\n" : "\n---\n";
        landing = static_cast<int>(snippet.size());
    } else {
        return false;
    }
    // kind == 3（引用）已在上面委托给 applyLinePrefix 并自带一条记录：这里只能在
    // 返回之后再开捕获，否则外层 span（光标一个点）盖不住行前缀改动。
    InputModel::beginEdit(input, cursor, cursor);
    input.text.insert(static_cast<std::size_t>(cursor), snippet);
    input.cursor = cursor + landing;
    input.selectionStart = input.cursor;
    input.selectionEnd = input.cursor;
    input.hasPreferredCursorX = false;
    InputModel::endEdit(input);
    return true;
}

// 执行菜单里排队的编辑命令（撤销/重做/剪切/复制/全选/任务勾选翻转）。
//
// 两个必须知道的点：
//   1. 只能在 compose 里调——要写输入组件的内部状态，而它只从 Ui 拿得到；
//      菜单回调发生在事件处理阶段，所以命令先排队，到这里才执行。
//   2. 改完必须让 doc.text 与输入组件的 text 立刻一致，否则下一次 build() 会
//      判定"外部值变了"把光标甩到文末（同 resetEditorInputState 的理由，§3.2）。
inline void applyEditorCommand(eui::Ui& ui, AppState& appState) {
    if (appState.unavailable) { appState.pendingEditorCommand = EditorCommand::None; return; }
    if (appState.pendingEditorCommand == EditorCommand::None) {
        return;
    }
    if (appState.pendingEditorCommand == EditorCommand::ToggleFold ||
        appState.pendingEditorCommand == EditorCommand::UnfoldAt) {
        // 折叠命令要查 lp_plan（本层看不见，且 lp 反向依赖本层，不能包含）——
        // 原样放行，由 editorView 的折叠处理块执行；这里**不清**队列。
        return;
    }
    // Do not mutate the committed UTF-8 text while IME owns a provisional range.
    // Keep the command queued; apply it on the first compose after composition ends.
    if (appState.pendingEditorCommand == EditorCommand::CleanAi ||
        appState.pendingEditorCommand == EditorCommand::InsertImageLink) {
        using InputModel = components::input_detail::InputModel;
        auto& composingState = ui.state<InputModel::InputState>(editorInputId(appState));
        if (!composingState.compositionText.empty()) {
            return;
        }
    }
    const EditorCommand command = appState.pendingEditorCommand;
    appState.pendingEditorCommand = EditorCommand::None;

    using InputModel = components::input_detail::InputModel;
    auto& inputState = ui.state<InputModel::InputState>(editorInputId(appState));
    // 文本有没有变先看 textRevision（每次实际改动恰好 bump 一次，见 InputModel::endEdit /
    // applyEditRecord），不再拷一份全文来比 —— 撤销自己也 bump，正是下面回写 doc.text 要的信号。
    // 命令尾巴上还有 abortEdit 的补账与两道 O(1) 兜底，判据的完整版见函数末尾的回写条件。
    const unsigned long long revisionBefore = inputState.textRevision;

    switch (command) {
        case EditorCommand::Undo:
            InputModel::undoEdit(inputState);
            break;
        case EditorCommand::Redo:
            InputModel::redoEdit(inputState);
            break;
        case EditorCommand::Copy:
            InputModel::copySelection(inputState);
            break;
        case EditorCommand::Cut:
            if (InputModel::hasTextSelection(inputState)) {
                InputModel::copySelection(inputState);
                InputModel::eraseSelection(inputState);
            }
            break;
        case EditorCommand::SelectAll:
            inputState.selectionStart = 0;
            inputState.selectionEnd = static_cast<int>(inputState.text.size());
            inputState.cursor = inputState.selectionEnd;
            inputState.dragAnchor = inputState.cursor;
            inputState.hasPreferredCursorX = false;
            break;
        case EditorCommand::ToggleTask:
            flipTaskCheckboxAt(inputState, appState.pendingTaskByte);
            appState.pendingTaskByte = -1;
            break;
        case EditorCommand::Paste: {
            // 统一粘贴入口（R2）：文本优先；剪贴板没有文本而只有位图时转图片
            // 附件流程——IO 不能在 compose 里做，这里只置位，由每帧 tick 消费。
            const std::string clipboardText =
                core::window::clipboardText(core::window::mainWindowHandle());
            if (!clipboardText.empty()) {
                InputModel::insertAtCursor(inputState, InputModel::filteredText(clipboardText, true));
            } else {
                appState.pendingImagePaste = neo::clipboardimage::available();
                if (appState.pendingImagePaste) {
                    app::requestUpdate();
                }
            }
            break;
        }
        case EditorCommand::InsertImageLink:
            // 载荷由事件阶段的 pasteImageAsAttachment 生成（PNG 已落盘），
            // 这里只是把链接按一次编辑插进光标处（有选区则替换）。
            InputModel::insertAtCursor(inputState, appState.pendingImageLink);
            appState.pendingImageLink.clear();
            break;
        case EditorCommand::FormatInline:
            switch (appState.pendingFormatKind) {
                case 1: applyInlineFormat(inputState, "**"); break;
                case 2: applyInlineFormat(inputState, "*"); break;
                case 3: applyInlineFormat(inputState, "~~"); break;
                case 4: applyInlineFormat(inputState, "=="); break;
                case 5: applyInlineFormat(inputState, "`"); break;
                case 6: applyInlineFormat(inputState, "$"); break;
                case 7: applyInlineFormat(inputState, "%%"); break;
                case 8: clearInlineFormat(inputState); break;
                default: break;
            }
            appState.pendingFormatKind = 0;
            break;
        case EditorCommand::SetHeading:
            // 标题 = "#"*n + " "（stripHeading 清旧标记）；正文 = 前缀空、只清旧标记。
            applyLinePrefix(inputState,
                            appState.pendingHeadingLevel > 0
                                ? std::string(static_cast<std::size_t>(appState.pendingHeadingLevel), '#') + " "
                                : std::string{},
                            true);
            appState.pendingHeadingLevel = 0;
            break;
        case EditorCommand::InsertBlock:
            if (appState.pendingBlockKind == 5) {
                applyLinkFormat(inputState, i18n::tr("safety.link_path"));
            } else if (appState.pendingBlockKind == 6) {
                applyLinkFormat(inputState, "https://");
            } else {
                insertBlockTemplate(inputState, appState.pendingBlockKind);
            }
            appState.pendingBlockKind = 0;
            break;
        case EditorCommand::EditLink:
            applyEditLink(inputState, appState.pendingLinkBeg, appState.pendingLinkEnd,
                          appState.pendingLinkUrl);
            appState.pendingLinkBeg = -1;
            appState.pendingLinkEnd = -1;
            appState.pendingLinkUrl.clear();
            break;
        case EditorCommand::CleanAi: {
            const int begin = orderedSelectionBegin(inputState);
            const int end = orderedSelectionEnd(inputState);
            const bool hasSelection = begin != end;
            const std::size_t cleanBegin = hasSelection ? static_cast<std::size_t>(begin) : 0u;
            const std::size_t cleanEnd = hasSelection ? static_cast<std::size_t>(end) : inputState.text.size();
            const cleanai::Result cleaned = cleanai::clean(inputState.text, cleanBegin, cleanEnd);
            if (cleaned.changed) {
                InputModel::beginEdit(inputState, 0, static_cast<int>(inputState.text.size()));
                inputState.text = cleaned.text;
                inputState.selectionStart = static_cast<int>(cleaned.selectionBegin);
                inputState.selectionEnd = static_cast<int>(cleaned.selectionEnd);
                inputState.cursor = inputState.selectionEnd;
                inputState.dragAnchor = inputState.cursor;
                inputState.hasPreferredCursorX = false;
                InputModel::endEdit(inputState);
                // Fold keys are byte offsets. A whole-document cleanup can shift them,
                // so clear folds and force decorations to rebuild for the new text.
                appState.foldedHeadings.clear();
                ++inputState.decorationRevision;
            }
            break;
        }
        case EditorCommand::ToggleFold:
        case EditorCommand::UnfoldAt:
            break;  // 到不了（函数开头已直通返回），仅为 switch 穷尽
        case EditorCommand::None:
            return;
    }

    // 兜底（不代替命令自己的 endEdit）：命令若漏了 endEdit，编辑深度会卡在 >0，
    // 之后每一次编辑都会并进这条陈旧捕获、整条撤销链被污染。这里把捕获收回 0，
    // 让失衡最多丢一条记录，而不是毁掉全部历史。
    // abortEdit 顺带做真实文本对照并报告"捕获期间文本确实变了"（它已为此补 bump 了
    // textRevision —— 排版缓存、装饰链、dirtyKey 同样只认 revision，组件侧必须自己失效）。
    const bool abortedText = InputModel::abortEdit(inputState);

    // 撤销可能把光标带到很远处，视图要跟着回去。
    inputState.followCaret = true;

    // 回写 doc.text 的判据：revision 快路径 + 两道 O(1) 兜底，**不做全文 memcmp**。
    //   · textRevision != revisionBefore —— 正常路径：endEdit / applyEditRecord 每次实际
    //     改动恰好 bump 一次（undo/redo 自己也 bump，正是这里要的信号）；abortEdit 的补
    //     bump 也落在这一条里。
    //   · abortedText —— 漏 endEdit 的改动。上面那次补 bump 已经覆盖它，这里并进条件是
    //     把"为什么必须同步"显式钉住，不依赖"abortEdit 一定会 bump"这个实现细节。
    //   · 长度差 —— 连捕获都没有的裸改（未来命令的漏网），动了长度就必然暴露。反向情况
    //     （doc 比组件新、于是误回写）不存在：所有改写 doc.text 的地方都同时置
    //     editorStateDirty，那一帧 compose 已在本函数之前把组件重置并清空命令队列
    //     （app.cpp 的 editorStateDirty 块），根本走不到这里。
    // 为什么不加全文比较（频率 / 代价）：本函数只在**有排队命令**的 compose 上真正干活
    // （开头 None 即返回），命令来自菜单 / 右键点击，用户速率 ≤ ~10 次/秒，不是 90fps 的
    // 每帧路径；即便如此 1MB 全文 memcmp ≈ 50µs、10MB ≈ 0.5ms，Copy / SelectAll 这类纯
    // 光标命令每次白付一次不划算。剩下的盲区只有"同长度 + 无捕获 + 无 bump 的裸改"——
    // 现有漏斗（beginEdit/endEdit 成对，或直接 bump）都到不了那里；组件 build 每帧本来
    // 就做一次 state.text != value 的全文比较（input.h），在那里兜全才是重复劳动。
    if (inputState.textRevision != revisionBefore || abortedText ||
        inputState.text.size() != appState.doc.text.size()) {
        // textRevision 的推进已经由记录提交/撤销/abortEdit 补账完成，这里只负责回写 ——
        // 再 bump 一次会让"每次实际改动恰好一次"变成两次（排版缓存判等不受影响，但计数会漂）。
        appState.doc.text = inputState.text;
        ++appState.revision;
        appState.recovered = false;
        maybeWriteRecovery(appState);
    }
}

} // namespace neo
