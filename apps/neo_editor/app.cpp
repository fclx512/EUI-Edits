#include "eui_neo.h"

#include "platform/native_dialogs.h"
#include "platform/single_instance.h"
#include "platform/font_safety.h"
#include "platform/vault_watcher.h"
#include "model/text_file.h"
#include "model/theme_loader.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "state/session_writer.h"
#include "state/tabs_trace.h"

#include "ui/context_menu.h"
#include "ui/editor_view.h"
#include "ui/find_bar.h"
#include "ui/image_preview.h"
#include "ui/menu_bar.h"
#include "ui/metrics.h"
#include "ui/overlays.h"
#include "ui/outline_view.h"
#include "ui/settings_panel.h"
#include "ui/status_bar.h"
#include "ui/vault_menu.h"
#include "ui/vault_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
// 命令行文档路径（双击 .txt/.md 启动）要用 GetCommandLineW / CommandLineToArgvW。
#include <windows.h>
#include <shellapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#endif
#endif

namespace app {
namespace {

const char* defaultTextFont() {
#if defined(_WIN32)
    // 仓库自带的默认字体是 Bold 显示体，界面文字换成系统正文字体才像工具。
    return "C:/Windows/Fonts/msyh.ttc";
#else
    return "";
#endif
}

// 命令行里的文档路径（双击 .txt/.md / "打开方式"启动）：取第一个真实存在的文件。
// 文件关联注册后 shell 会以 `"exe" "文档路径"` 启动进程，这个参数不接的话
// 双击打开的永远是上次的会话文档——注册功能等于摆设。
std::string commandLineDocumentPath() {
#if defined(_WIN32)
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return {};
    }
    std::string result;
    for (int index = 1; index < argumentCount; ++index) {
        const std::string candidate = neo::textfile::pathToUtf8(arguments[index]);
        std::error_code error;
        if (std::filesystem::is_regular_file(neo::textfile::pathFromUtf8(candidate), error)) {
            result = candidate;
            break;
        }
    }
    LocalFree(arguments);
    return result;
#else
    return {};
#endif
}

// 启动时恢复上次的会话。对话框与文件 IO 都不在这里做，避免启动卡顿。
void restoreSession(neo::AppState& state) {
    neo::sessionRecoveryWriter() = &neo::persistDocumentSession;
    // 会话写的 commit ack（主线程）：只有完整会话提交成功才清旧单页恢复并消警告；
    // 失败保留上一完整快照与内存脏稿，给持续提示（不再无声吞错）。
    neo::sessionwriter::setCommitCallback([](bool ok, std::uint64_t) {
        neo::AppState& current = neo::state();
        if (ok) {
            neo::settings::clearRecovery();
            current.recoveryWriteWarning = false;
        } else if (!current.recoveryWriteWarning) {
            current.recoveryWriteWarning = true;
            neo::showToast(current, neo::i18n::tr("safety.save_failed"),
                           neo::i18n::tr("tabs.recovery_write_failed"));
        }
    });
    neo::settings::Data& saved = neo::settings::current();
    neo::i18n::initialize(saved.uiLanguage);
    std::string fontMessage;
    neo::fontsafety::recoverSession(saved, fontMessage);
    bool fontChanged = false;
    const auto restoreFont = [&](std::string& path, bool requireMonospace) {
        if (path.empty()) return;
        const auto result = neo::fontsafety::validate(path);
        if (!result.ok || (requireMonospace && !result.monospace)) {
            fontMessage += (fontMessage.empty() ? "" : " ") +
                neo::i18n::format("safety.font_fallback_file", {{"file", neo::textfile::fileName(path)}});
            path.clear();
            fontChanged = true;
        }
    };
    restoreFont(saved.editorFontFile, false);
    restoreFont(saved.uiFontFile, false);
    restoreFont(saved.codeFontFile, true);
    std::string armError;
    if (!neo::fontsafety::armSession(saved, armError)) {
        saved.editorFontFile.clear();
        saved.uiFontFile.clear();
        saved.codeFontFile.clear();
        fontChanged = true;
        fontMessage = neo::i18n::format("safety.font_fallback", {{"detail", armError}});
    }
    if (fontChanged) neo::settings::flush();
    if (!fontMessage.empty()) neo::showToast(state, neo::i18n::tr("safety.font_recovered"), fontMessage);
    // 文档库不是常驻仓库（2026-09-26 口径）：根目录由"当前打开的文档"决定，
    // 因此这里不恢复 saved.vault —— 它会在下面按最终加载到的文档重设。
    state.mode = saved.mode == 1 ? neo::EditorMode::Vault : neo::EditorMode::Simple;
    state.showStatusBar = saved.showStatusBar;
    state.showLineNumbers = saved.lineNumbers;
    state.readableWidth = saved.readableWidth;
    state.animations = saved.animations;
    state.theme = saved.theme == 1 ? neo::ThemeMode::Light : neo::ThemeMode::Dark;
    state.uiScale = std::clamp(saved.uiScale, neo::kMinimumUiScale, neo::kMaximumUiScale);
    state.editorFontSize =
        std::clamp(saved.editorFontSize, neo::kMinimumEditorFontSize, neo::kMaximumEditorFontSize);
    state.uiFontSize =
        std::clamp(saved.uiFontSize, neo::kMinimumUiFontSize, neo::kMaximumUiFontSize);
    state.vaultWidth = std::clamp(saved.vaultWidth, neo::kMinimumVaultWidth, neo::kMaximumVaultWidth);

    // 自选字体。原先这两个字段漏了恢复：设置里存了、面板里选了、但重启后不回填，
    // 于是每次启动都退回默认字体；更糟的是此后任何一次 persistSettings 都会把
    // settings.ini 里的字体路径覆盖成空，等于把用户的选择抹掉。
    state.editorFontFile = saved.editorFontFile;
    state.uiFontFile = saved.uiFontFile;
    state.codeFontFile = saved.codeFontFile;

    // 系统缩放。必须在 GLFW 初始化之后问——DSL 入口是在 glfwInit() 之后才第一次
    // 读配置的，所以这里已经带 DPI 感知；否则拿到的永远是 100%，窗口会偏小。
    state.systemScale = neo::dialogs::systemScale();

    // Crash recovery stays a dirty draft; an explicit startup file must pass
    // the same save/discard/cancel gate before replacing it.
    neo::restoreStartupDocument(state, commandLineDocumentPath());

    if (state.vaultRoot.empty() && !state.path.empty()) {
        state.vaultRoot = neo::textfile::parentPath(state.path);
        state.vaultAddress = state.vaultRoot;
    }
    neo::mergeRelatedVaultRoots(state);

    if (!state.vaultRoot.empty()) {
        neo::refreshVault(state, true);
    } else {
        // 没有根就不扫：清掉可能残留的旧行，侧栏显示空提示。
        state.vaultScan.reset();
        state.vaultRowsGeneration = 0;
        state.rows.clear();
    }
}

// 应用级快捷键。控件没消费的按键才会走到这里（input 会自己吃掉编辑类快捷键）。
void handleShortcut(neo::AppState& state, const eui::KeyEvent& event) {
    if (state.savePickerOpen || state.sessionClosePending || state.closeApproved) return;
    if (!event.isDown()) {
        return;
    }

    if (state.saveConflictOpen || state.pending != neo::PendingAction::None) {
        if (event.key == eui::InputKey::Escape) {
            if (state.saveConflictOpen) neo::resolveSaveConflict(state, 2);
            else neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Cancel);
        }
        return;
    }
    if(state.riskAction!=neo::AppState::RiskAction::None) {
        if(event.key==eui::InputKey::Escape) {neo::cancelRisk(state);requestUpdate();}
        return;
    }
    if (state.vaultPromptKind != neo::VaultPromptKind::None) {
        if (event.key == eui::InputKey::Escape) neo::cancelVaultPrompt(state);
        return;
    }
    // F2 is handled only by the focused library row. A saved selection alone
    // must never rename an old row while the document/address/filter has focus.
    if (event.key == eui::InputKey::F2) return;
    // 不带修饰键的功能键
    if (!event.modifiers.shortcut() && !event.modifiers.alt &&
        event.key == eui::InputKey::F3) {
        if (state.settingsOpen) {
            return;
        }
        if (state.findOpen) {
            neo::queueFindAction(state, event.modifiers.shift
                                            ? neo::FindAction::Previous : neo::FindAction::Next);
        } else {
            neo::openFindBar(state);
        }
        return;
    }
    if (!event.modifiers.shortcut() && !event.modifiers.alt && !event.modifiers.shift) {
        switch (event.key) {
            case eui::InputKey::Escape:
                // 全屏图片预览最优先（编辑区聚焦时组件已把 Esc 放行到应用级），
                // 然后是设置面板、菜单；都没有打开时什么都不做，
                // 免得把 Esc 从输入法取消预编辑的手里抢走。
                if (!state.imagePreviewPath.empty()) {
                    state.imagePreviewPath.clear();
                    state.imagePreviewWidth = 0.0f;
                    state.imagePreviewHeight = 0.0f;
                    state.imageViewport = neo::ImageViewport{};
                    state.findEditorFocusPending = true;
                    requestUpdate();
                } else if (state.pending!=neo::PendingAction::None) {
                    state.pending=neo::PendingAction::None;state.pendingNewMarkdown=false;requestUpdate();
                } else if (state.languageMenuOpen) {
                    state.languageMenuOpen=false;requestUpdate();
                } else if (state.settingsOpen) {
                    state.settingsOpen = false;
                    requestUpdate();
                } else if (state.contextMenuOpen || state.vaultContextMenuOpen) {
                    state.contextMenuOpen = false;
                    state.vaultContextMenuOpen = false;
                    requestUpdate();
                } else if (state.findOpen) {
                    neo::dismissFindOnEscape(state);
                } else if (state.openMenu != neo::MenuKind::None) {
                    state.openMenu = neo::MenuKind::None;
                    requestUpdate();
                } else if (state.linkEditorOpen || state.vaultPromptKind != neo::VaultPromptKind::None ||
                           state.vaultDeletePending) {
                    // 模态弹窗（编辑链接 / 文档库新建-重命名 / 删除确认）。
                    state.linkEditorOpen = false;
                    state.vaultPromptKind = neo::VaultPromptKind::None;
                    state.vaultDeletePending = false;
                    requestUpdate();
                } else if (state.contextMenuOpen || state.vaultContextMenuOpen) {
                    // 右键菜单（弹层自带点外关闭，Esc 也收掉）。
                    state.contextMenuOpen = false;
                    state.vaultContextMenuOpen = false;
                    requestUpdate();
                }
                return;
            default:
                break;
        }
    }

    if (!event.modifiers.shortcut() || event.modifiers.alt) {
        return;
    }

    switch (event.key) {
        case eui::InputKey::S:
            if (event.modifiers.shift) {
                neo::saveDocumentAs(state);
            } else {
                neo::saveDocument(state);
            }
            break;
        case eui::InputKey::O:
            if (!neo::documentModalOpen(state)) neo::openFileFromDialog(state);
            break;
        case eui::InputKey::N:
            if (!neo::documentModalOpen(state)) neo::newDocument(state);
            break;
        case eui::InputKey::W:
            neo::requestCloseTab(state, state.tabId);
            break;
        case eui::InputKey::Tab: {
            const auto it = std::find(state.tabOrder.begin(), state.tabOrder.end(), state.tabId);
            if (it != state.tabOrder.end() && !state.tabOrder.empty()) {
                const auto index = static_cast<std::size_t>(it - state.tabOrder.begin());
                const auto count = state.tabOrder.size();
                neo::activateDocumentTab(state, state.tabOrder[(index + (event.modifiers.shift ? count - 1 : 1)) % count]);
            }
            break;
        }
        case eui::InputKey::Digit1: case eui::InputKey::Digit2: case eui::InputKey::Digit3:
        case eui::InputKey::Digit4: case eui::InputKey::Digit5: case eui::InputKey::Digit6:
        case eui::InputKey::Digit7: case eui::InputKey::Digit8: case eui::InputKey::Digit9: {
            const auto number = static_cast<std::size_t>(event.key) - static_cast<std::size_t>(eui::InputKey::Digit1);
            if (!state.tabOrder.empty()) {
                const auto index = number == 8 ? state.tabOrder.size() - 1 : number;
                if (index < state.tabOrder.size()) neo::activateDocumentTab(state, state.tabOrder[index]);
            }
            break;
        }
        case eui::InputKey::Comma:
            state.settingsOpen = !state.settingsOpen;
            requestUpdate();
            break;
        case eui::InputKey::F:
            if (!state.settingsOpen) {
                neo::openFindBar(state);
            }
            break;
        case eui::InputKey::H:
            if (!state.settingsOpen) neo::openFindBar(state, true);
            break;
        case eui::InputKey::G:
            if (state.findOpen) {
                neo::queueFindAction(state, event.modifiers.shift
                                                ? neo::FindAction::Previous : neo::FindAction::Next);
            }
            break;
        case eui::InputKey::J:
            // Ctrl+Shift+J = 折叠/展开光标所在章节（S3f 批次 C）。原定 Ctrl+Shift+F
            // 在 OS 层被别的进程的低级键盘钩子整链吞掉（F 的 keydown 永远到不了应用，
            // 同会话对照 Ctrl+Shift+J 正常到达，故换 J）。裸 Ctrl+F 留给将来的搜索，
            // 这里不消费。
            if (event.modifiers.shift) {
                state.pendingEditorCommand = neo::EditorCommand::ToggleFold;
            }
            break;
        default:
            break;
    }
}

} // namespace

// 单实例闸门注册。main()（glue glfw_app_main）在 glfwInit 之前调 app::singleInstanceGate()，
// 默认恒放行；EUI-Edits 在静态初始化时把自己的实现挂上去——第二实例在 glfwInit 之前
// 就完成"转发命令行文档 + 置前已有实例"并零成本退出，主实例则启动监听线程（收到
// SetEvent 后 requestUpdate 唤醒按需渲染的 compose，tick 消费转发的路径）。
// 用注册而非 main 直接引用强符号：framework 的 main 是所有示例 app 共享的。
namespace {
const bool g_singleInstanceWired = [] {
    app::detail::setSingleInstanceGate([] {
        const int savePicker = neo::dialogs::runSaveDialogHelperIfRequested();
        if (savePicker >= 0) std::exit(savePicker);
        const int fontProbe = neo::fontsafety::probeCommandLineIfRequested();
        if (fontProbe >= 0) std::exit(fontProbe);
        // 探针/自动化旁路：NEO_SINGLE_INSTANCE=0 时不抢互斥、不转发、不置前，
        // 配合隔离的 APPDATA 允许与用户手上的实例并存（GUI 探针在用户验证期间也能跑）。
        if (const char* bypass = std::getenv("NEO_SINGLE_INSTANCE")) {
            if (bypass[0] == '0') {
                return true;
            }
        }
        return neo::platform::singleInstanceAcquire(
            [] { requestUpdate(); }, commandLineDocumentPath());
    });
    return true;
}();
} // namespace

// 全应用唯一的配置对象。框架要求 dslAppConfig() 返回 const 引用，而界面缩放
// 要能在运行时改，所以保留这个可写入口给 neo::mutableAppConfig()。
DslAppConfig& appConfigStorage() {
    static DslAppConfig config = [] {
        neo::AppState& state = neo::state();
        restoreSession(state);

        // 主题文件（T13）：开机恢复 last_theme_file，**必须排在下面
        // .clearColor(neo::editorColors().window) 之前** —— editorColors() 按
        // themeRevision() 失效，晚于取色加载就会让首帧窗口底色停在内置配色。
        // 文件不存在 / JSON 坏 / 版本不认识由 loader 回退内置并给出人话说明，
        // 启动绝不因主题失败（说明走启动后的 toast）。
        const std::string& savedThemeFile = neo::settings::current().lastThemeFile;
        if (!savedThemeFile.empty()) {
            // 路径**无论成败都记下**：加载失败时配色回退内置，但不能顺手把用户
            // 的主题引用从 settings 里抹掉（盘符没插上时，下次 persist 就丢了）；
            // 设置面板那行照样显示它，用户可以"更换"或点"内置"清掉。
            state.themeFile = savedThemeFile;
            std::string themeError;
            if (neo::themeloader::load(savedThemeFile, themeError)) {
                // 主题只覆盖 base 那一侧的配色，外观跟着切过去（正常情况下
                // persistSettings 落的 theme 已经等于它，这里兜住手改 settings.ini）。
                if (neo::activeTheme().version == neo::themeloader::kSchemaVersion) {
                    state.theme = neo::activeTheme().baseLight ? neo::ThemeMode::Light
                                                              : neo::ThemeMode::Dark;
                }
            } else {
                neo::showToast(state, neo::i18n::tr("safety.theme_unavailable"), themeError);
            }
        }

        // windowSize 是物理像素，而逻辑空间 = 物理 / (dpiScale * uiScale)。
        // 要让不同系统缩放的机器上"能看到的文字量"一致，物理尺寸就得跟着系统缩放走：
        // 896x608 是本机 125% 下的逻辑面积，也就是原来那个 1120x760 物理窗口。
        const float scale = state.systemScale > 0.0f ? state.systemScale : 1.0f;
        const auto scaled = [scale](float logical) {
            return static_cast<int>(std::lround(logical * scale));
        };

        return DslAppConfig{}
            .title("EUI-Edits")
            .pageId("neo_editor")
#if defined(EUI_EDITS_BUNDLED_RESOURCES)
            // The Win32 class uses the embedded ICO, including its small frame.
            .iconPath("")
#endif
            .clearColor(neo::editorColors().window)
            .windowSize(scaled(896.0f), scaled(608.0f))
            .minWindowSize(scaled(608.0f), scaled(384.0f))
            // 跟随显示器刷新率；空闲时仍按需渲染，不会主动忙等到 240 Hz。
            .fps(0.0)
            // 界面字体（框架全局）必须走 config 传：框架在 initialize() 里会用
            // 这里的值覆盖一次全局字体，启动时直接调 setDefaultFontFiles 会被它盖掉
            // （运行时从设置面板换字体没这个问题，那条路是 initialize 之后调的）。
            .textFont(state.uiFontFile.empty() ? std::string(defaultTextFont()) : state.uiFontFile)
            .uiScale(state.uiScale)
            // 反馈要求"输入区 = 文本选择、其余 = 默认箭头"：文本输入显式 IBeam，
            // 其余可点元素（菜单/按钮/侧栏行）的"手型"统一映射成箭头。
            .interactiveCursor(eui::CursorShape::Arrow)
            .onKeyEvent([](const eui::KeyEvent& event) { handleShortcut(neo::state(), event); })
            .onCloseRequest([] { return neo::requestCloseDocument(neo::state()); })
            .onShutdown([] {
                // Async workers have already stopped here. Approved WM_CLOSE has
                // completed session cleanup; submitting a write here cannot run
                // and used to wait for the entire 5-second checkpoint timeout.
                neo::settings::flush();
                neo::fontsafety::cleanSession();
                neo::tracelog::flush();
                neo::platform::singleInstanceShutdown();
            });
    }();
    return config;
}

const DslAppConfig& dslAppConfig() {
    return appConfigStorage();
}

// 文档库实时刷新：监视线程发现文件系统变动就回调 requestUpdate 唤醒主循环
// （框架按需渲染，空闲时没有别的帧可搭车），这里每帧比对计数，变动后防抖 0.8s
// 再重扫（外部写一批文件只扫一次），不重置滚动与展开状态。

neo::platform::VaultWatcher& vaultWatcher() {
    static neo::platform::VaultWatcher watcher;
    static bool notifyWired = false;
    if (!notifyWired) {
        notifyWired = true;
        watcher.onChange([] { app::requestUpdate(); });
    }
    return watcher;
}

void vaultWatchTick(neo::AppState& state) {
    static std::uint64_t lastSeen = 0;
    static std::chrono::steady_clock::time_point pendingSince{};
    static bool pending = false;
    static std::string watchedRoot;
    if (watchedRoot != state.vaultRoot) {
        watchedRoot = state.vaultRoot; pending = false;
        vaultWatcher().watch(state.vaultRoot);
        lastSeen = vaultWatcher().changeCount();
    }
    const std::uint64_t seen = vaultWatcher().changeCount();
    if (seen != lastSeen) {
        lastSeen = seen;
        if (!pending) {
            pending = true;
            pendingSince = std::chrono::steady_clock::now();
        }
    }
    if (pending) {
        const auto elapsed = std::chrono::steady_clock::now() - pendingSince;
        if (elapsed >= std::chrono::milliseconds(800)) {
            pending = false;
            if (!state.vaultRoot.empty()) {
                // 目录事件后**后台**重扫，不阻塞 UI；完成后由 tickVaultScan 采纳新代次。
                neo::requestVaultRefresh(state);
            }
        } else {
            // 防抖窗口内继续要帧，否则空闲时没有下一帧来兑现这次刷新。
            app::requestUpdate();
        }
    }
    // 每帧轻量采纳共享扫描快照（扫描完成/切页后各发生一次），代次没变时是空操作。
    neo::tickVaultScan(state);
}

// 大纲在 editorView 完成装饰/解析之后投影既有计划，绝不抢在编辑器之前调用
// cachedPlan：那会把带编辑 delta 的增量重建链截断，退成整篇重解析。
void refreshOutlineFromEditor(neo::AppState& state) {
    if (state.vaultTab != neo::VaultTab::Outline || state.mode != neo::EditorMode::Vault) {
        return;
    }
    if (!state.markdownCapable()) {
        if (!state.outline.empty()) {
            state.outline.clear();
            app::requestUpdate();
        }
        state.outlinePlanVersion = static_cast<unsigned long long>(-1);
        return;
    }
    const neo::lp::PlanCache& cache = neo::lp::planCache();
    if (!cache.valid || cache.text != state.doc.text ||
        state.outlinePlanVersion == cache.version) {
        return;
    }
    state.outline = neo::outlineFromPlan(state.doc.text, cache.plan);
    state.outlinePlanVersion = cache.version;
    app::requestUpdate();  // 本帧侧栏已构建，下一帧展示新目录。
}

void applyOutlineJump(eui::Ui& ui, neo::AppState& state) {
    if (state.pendingOutlineJumpByte < 0) {
        return;
    }
    using InputModel = components::input_detail::InputModel;
    auto& input = ui.state<InputModel::InputState>(neo::editorInputId(state));
    const int target = InputModel::clampUtf8Boundary(
        input.text, std::clamp(state.pendingOutlineJumpByte, 0, static_cast<int>(input.text.size())));
    state.pendingOutlineJumpByte = -1;

    // 目标标题可能藏在已折叠的父章节中，只展开它的祖先。
    const neo::lp::PlanCache& cache = neo::lp::planCache();
    if (cache.valid && cache.text == state.doc.text) {
        const int lineIndex = cache.plan.lineIndexFor(target);
        const neo::LpLine* line = cache.plan.lineAt(lineIndex);
        int ancestor = line != nullptr ? line->parentHeadingLine : -1;
        bool unfolded = false;
        while (ancestor >= 0) {
            const neo::LpLine* parent = cache.plan.lineAt(ancestor);
            if (parent == nullptr) {
                break;
            }
            unfolded |= state.foldedHeadings.erase(parent->srcBeg) > 0;
            ancestor = parent->parentHeadingLine;
        }
        if (unfolded) {
            ++input.decorationRevision;
        }
    }
    input.cursor = target;
    input.selectionStart = target;
    input.selectionEnd = target;
    input.dragAnchor = target;
    input.selecting = false;
    input.hasPreferredCursorX = false;
    input.followCaret = true;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    neo::AppState& state = neo::state();
    const neo::EditorColors& colors = neo::editorColors();
    const neo::UiMetrics metrics = neo::uiMetrics(state);
    if (state.sessionClosePending) {
        // The old editable tree must disappear while final cleanup is in flight.
        // Keep painting and processing window messages instead of blocking WM_CLOSE.
        ui.requestFocus("window.closing");
        ui.rect("window.closing").size(screen.width, screen.height).color(colors.editor).build();
        ui.text("window.closing.label").position(24, 24).size(screen.width - 48, 40)
            .text(neo::i18n::tr("safety.closing"))
            .fontFamily(neo::uiFontFamily(state)).fontSize(metrics.panelFontSize)
            .color(colors.text).build();
        return;
    }
    // 分段计时（NEO_TABS_TRACE 关闭时是廉价空操作）：把一帧拆成
    //   frame-begin(gap) → sync-inputs → editor-view(layout/decorations/compose)
    //   → outline / vault-panel → compose(整帧)。
    // "请求→下一帧开始"的排队间隔只挂在切页后第一帧上，避免每帧都算。
    const std::uint64_t frameSeq = neo::tracelog::nextFrameSequence();
    const std::uint64_t switchAt = neo::tracelog::switchRequestMicros();
    if (switchAt != 0) {
        if (neo::tracelog::enabled()) {
            neo::tracelog::event("frame-begin", 0,
                                 "gap_us=" + std::to_string(neo::tracelog::nowMicros() - switchAt) +
                                     " frame=" + std::to_string(frameSeq));
        }
        neo::tracelog::clearSwitchRequest();
    }
    neo::tracelog::Span composeSpan("compose");
    // 图片粘贴（R2）：compose 里不做 IO，但附件落盘的 tick 与 vaultWatchTick
    // 同位（既有先例：文档库重扫也在这里），链接插入仍走本帧后面的 applyEditorCommand。
    if (state.pendingImagePaste) {
        neo::pasteImageAsAttachment(state);
    }
    vaultWatchTick(state);
    for(const auto& path:core::window::consumeDroppedPaths(core::window::mainWindowHandle())) {
        if (neo::documentModalOpen(state)) continue;
        if(state.settingsOpen) {neo::showToast(state,neo::i18n::tr("safety.return_editor"),neo::i18n::tr("safety.drop_return"));continue;}
        neo::requestOpenPath(state,path);
    }

    // 单实例转发（二次启动把文档转给本进程）：监听线程只负责唤醒 UI，
    // 真正的文件读取在每帧 tick 里做；requestOpenPath 带脏文档确认，
    // 与"文件→打开"完全同链。没有请求时这是一次廉价的文件存在性检查。
    // Leave a forwarded request on disk until the current modal transaction
    // ends; consuming it now could replace the user's pending close/open choice.
    if (!neo::documentModalOpen(state)) {
        const std::string forwardedPath = neo::platform::takeSingleInstanceOpenPath();
        if (!forwardedPath.empty()) {
            // 转发打开 = 用户明确要这篇文档：设置页开着也让位回到编辑区
            // （拖拽进设置页仍走上面的拒绝+提示，行为不同是有意的）。
            if (state.settingsOpen) { state.settingsOpen = false; app::requestUpdate(); }
            neo::requestOpenPath(state, forwardedPath);
        }
    }

    {
        neo::tracelog::Span syncSpan("sync-inputs");
        neo::syncDocumentTabInputs(ui, state);
    }
    // 新建/打开文件后，把内容同步进输入组件的内部状态：光标回文首、滚动归零、撤销栈清空。
    if (state.editorStateDirty) {
        neo::resetEditorInputState(ui, state.doc.text);
        state.editorStateDirty = false;
        state.outline.clear();
        state.outlinePlanVersion = static_cast<unsigned long long>(-1);
        state.outlineScroll = 0.0f;
        state.pendingOutlineJumpByte = -1;
        state.findMatchesRevision = static_cast<unsigned long long>(-1);
        state.findCurrent = -1;
        // 刚换过文档，上一条编辑命令已经没意义了。
        state.pendingEditorCommand = neo::EditorCommand::None;
        state.pendingTaskByte = -1;
        // 预览的那张图可能属于上一篇文档，一并收掉。
        state.imagePreviewPath.clear();
        state.imagePreviewWidth = 0.0f;
        state.imagePreviewHeight = 0.0f;
        state.imageViewport = neo::ImageViewport{};
    }
    // 菜单里的撤销/重做/剪切/复制/全选在这里落到输入组件的内部状态上。
    neo::applyEditorCommand(ui, state);
    // 恢复写的"自动到期"：节流窗口内被推迟的修改在窗口到期后补提交一次。
    neo::tickRecoveryWrite(state);
    neo::applyFindState(ui, state);
    applyOutlineJump(ui, state);
    if (state.vaultTab == neo::VaultTab::Outline && state.mode == neo::EditorMode::Vault) {
        const int cursor = ui.state<components::input_detail::InputModel::InputState>(neo::editorInputId(state)).cursor;
        state.outlineCurrentByte = -1;
        for (const auto& entry : state.outline) {
            if (entry.byteOffset > cursor) break;
            state.outlineCurrentByte = entry.byteOffset;
        }
    }

    const bool vaultMode = state.mode == neo::EditorMode::Vault;
    const float panelWidth = vaultMode
        ? std::min(state.vaultWidth, std::max(0.0f, screen.width * 0.38f)) : 0.0f;
    const float dividerWidth = panelWidth > 0.0f ? 1.0f : 0.0f;
    const float statusHeight = state.showStatusBar ? metrics.statusBarHeight : 0.0f;
    const float bodyHeight = std::max(0.0f, screen.height - metrics.menuBarHeight - statusHeight);
    const float editorWidth = std::max(0.0f, screen.width - panelWidth - dividerWidth);

    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("root.bg")
                .fill()
                .ignoreLayout()
                .color(colors.window)
                .build();

            if (state.settingsOpen) {
                // 设置是同一主窗口内的完整页面。跳过编辑器树，避免背后编辑区可命中；
                // editor.input 的 retained UI state 仍留在 Ui::StateStore 中。
                neo::settingsPanelOverlay(ui, state, screen);
                neo::toastOverlay(ui, state, screen);
            } else {
                state.settingsOpenLast = false;
                ui.column("page")
                    .size(screen.width, screen.height)
                    .content([&] {
                        // 菜单栏本身画在最上层的 menuBarView 里（它要压过菜单弹层的
                        // 全屏关闭遮罩），这里只留出同高的位置。
                        ui.rect("page.menuBar")
                            .size(screen.width, metrics.menuBarHeight)
                            .color(colors.toolbar)
                            .build();

                        ui.row("body")
                            .size(screen.width, bodyHeight)
                            .gap(0.0f)
                            .content([&] {
                                if (panelWidth > 0.0f) {
                                    neo::tracelog::Span vaultSpan("vault-panel");
                                    neo::vaultPanelView(ui, state, panelWidth, bodyHeight);
                                    neo::dividerView(ui, "body.divider", bodyHeight);
                                }
                                // When the card spans almost the entire text viewport,
                                // keep search results visible underneath it.
                                const float findClearance = state.findOpen && editorWidth < 578.0f
                                    ? std::min(neo::findPanelHeight(state) + 24.0f,
                                               std::max(0.0f, bodyHeight - 80.0f)) : 0.0f;
                                ui.column("editor.region").size(editorWidth, bodyHeight).content([&] {
                                    if (findClearance > 0.0f)
                                        ui.rect("editor.find.clearance").size(editorWidth, findClearance)
                                            .color(colors.editor).build();
                                    {
                                        // editor-view 内包含 LP 计划/装饰快照（decorations）与
                                        // 整篇 InputLayout::build（layout）——从 app 侧无法再细分，
                                        // 探测时用暖/冷两轮之差把"重排整篇"从固定开销里分离出来。
                                        neo::tracelog::Span editorSpan("editor-view");
                                        neo::editorView(ui, state, metrics, editorWidth, bodyHeight - findClearance);
                                    }
                                }).build();
                                if (state.findEditorFocusPending) {
                                    state.findEditorFocusPending = false;
                                    ui.requestFocus(state.tabListOpen ? "menubar.tabs.list.keys"
                                                                      : neo::editorInputId(state) + ".hit");
                                }
                                {
                                    neo::tracelog::Span outlineSpan("outline");
                                    refreshOutlineFromEditor(state);
                                }
                            })
                            .build();

                        if (state.showStatusBar) {
                            neo::statusBarView(ui, state, screen.width);
                        }
                    })
                    .build();

                if (state.findOpen) {
                    const float findWidth = std::min(550.0f, std::max(280.0f, editorWidth - 28.0f));
                    neo::findBarView(ui, state, findWidth, screen.width - findWidth - 14.0f,
                                     metrics.menuBarHeight + 12.0f, screen);
                }
                // 弹层最后声明，保证盖在最上层。
                neo::toastOverlay(ui, state, screen);
                // 编辑区右键菜单（2026-09-25）：只开一条（state.contextMenuOpen），
                // 关闭遮罩全屏，点哪儿都收起。
                neo::editorContextMenuOverlay(ui, state, screen);
                // 文档库右键菜单（2026-09-26）与其"新建/重命名/删除"弹窗。
                neo::vaultContextMenuOverlay(ui, state, screen);
                neo::vaultPromptOverlay(ui, state, screen);
                neo::vaultDeleteOverlay(ui, state, screen);
                // "编辑链接"弹窗（右键菜单 → 编辑链接，2026-09-26）。
                neo::linkEditorOverlay(ui, state, screen);
                // 菜单栏最后画、zIndex 也最高：菜单展开时的全屏关闭遮罩在它下面，
                // "点另一个菜单直接切过去"才成立（详见 ui/menu_bar.h）。
                neo::menuBarView(ui, state, screen);

                // 全屏图片预览（S3f 批次 E）：一切之上的模态层 —— 打开时点哪儿都只
                // 关预览，不会误碰编辑区或菜单。
                neo::imagePreviewOverlay(ui, state, screen);
                neo::languageOverlay(ui,state,screen);
            }
        })
        .build();
    neo::riskOverlay(ui,state,screen);
    neo::confirmOverlay(ui, state, screen);
    neo::applyInteractionDefaults(ui, neo::uiFontFamily(state), neo::animationsEnabled(state));
}

} // namespace app

namespace neo {

app::DslAppConfig& mutableAppConfig() {
    return app::appConfigStorage();
}

} // namespace neo
