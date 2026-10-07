#pragma once

#include "model/i18n.h"

#include "components/contextmenu.h"
#include "model/link_target.h"
#include "model/lp_decorations.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <vector>

// 编辑区右键菜单（2026-09-25，对齐 Obsidian 的编辑器上下文菜单）：
// 链接 / 文本格式 ▸ / 段落设置 ▸ / 插入 ▸ / 剪切-复制-粘贴-全选。
// 复用 components::contextMenu 级联弹层（与菜单栏同一套），所以继承它的
// 菜单项在保持逻辑索引的同时支持分隔线、禁用态、勾选态和快捷键提示。
//
// 命令走 pendingEditorCommand 队列：回调在事件阶段、真正改文本在 compose
// （applyEditorCommand），与菜单栏"编辑"命令同一条路。
namespace neo {
namespace context_menu_detail {

using MenuItem = components::ContextMenuItem;

inline std::vector<MenuItem> editorContextMenuItems(bool onLink, bool hasSelection, int headingLevel) {
    std::vector<MenuItem> items;
    if (onLink) {
        // 光标落在链接上（2026-09-26）：Obsidian 的链接右键菜单 —— 打开在前，
        // 编辑紧随其后，其余通用项整体后移两位。
        items.emplace_back(i18n::tr("context.open_link"));
        items.emplace_back(i18n::tr("context.edit_link"));
    }
    items.push_back(MenuItem(i18n::tr("context.new_link")).withSeparatorBefore(onLink));
    items.push_back(MenuItem(i18n::tr("context.external_link")));
    // Obsidian 的"文本格式"八项原样保留：加粗/倾斜/删除线/高亮/代码/数学/注释/清除格式。
    items.push_back(MenuItem(i18n::tr("context.format"), {
        MenuItem(i18n::tr("context.bold")),
        MenuItem(i18n::tr("context.italic")),
        MenuItem(i18n::tr("context.strike")),
        MenuItem(i18n::tr("context.highlight")),
        MenuItem(i18n::tr("context.code")),
        MenuItem(i18n::tr("context.math")),
        MenuItem(i18n::tr("context.comment")),
        MenuItem(i18n::tr("context.clear_format")),
    }));
    items.push_back(MenuItem(i18n::tr("context.paragraph"), {
        MenuItem(i18n::tr("context.body")).withChecked(headingLevel == 0),
        MenuItem(i18n::tr("context.heading1")).withChecked(headingLevel == 1),
        MenuItem(i18n::tr("context.heading2")).withChecked(headingLevel == 2),
        MenuItem(i18n::tr("context.heading3")).withChecked(headingLevel == 3),
    }));
    items.push_back(MenuItem(i18n::tr("context.insert"), {
        MenuItem(i18n::tr("context.code_block")),
        MenuItem(i18n::tr("context.table")),
        MenuItem(i18n::tr("context.quote")),
        MenuItem(i18n::tr("context.divider")),
        // 选择导入图片（2026-10-06）：系统文件对话框选图，绝对路径引用。
        MenuItem(i18n::tr("context.image")),
    }));
    items.push_back(MenuItem(i18n::tr("context.cut")).withSeparatorBefore().withEnabled(hasSelection).withShortcut("Ctrl+X"));
    items.push_back(MenuItem(i18n::tr("context.copy")).withEnabled(hasSelection).withShortcut("Ctrl+C"));
    items.push_back(MenuItem(i18n::tr("context.paste")).withShortcut("Ctrl+V"));
    items.push_back(MenuItem(i18n::tr("context.select_all")).withShortcut("Ctrl+A"));
    return items;
}

// 右键实际命中的链接目标（菜单打开/分发时都按保存的命中字节重查）。
inline LinkTarget linkTargetUnderCursor(const AppState& state, eui::Ui& ui) {
    using InputModel = components::input_detail::InputModel;
    const InputModel::InputState& inputState = ui.state<InputModel::InputState>(editorInputId(state));
    if (inputState.contextLinkByte < 0) {
        return {};
    }
    return linkTargetAt(lp::cachedPlan(state.doc.text), state.doc.text, inputState.contextLinkByte);
}

inline bool editorHasSelection(eui::Ui& ui) {
    using InputModel = components::input_detail::InputModel;
    const InputModel::InputState& inputState = ui.state<InputModel::InputState>(editorInputId(neo::state()));
    return inputState.selectionStart != inputState.selectionEnd;
}

inline int currentHeadingLevel(const AppState& state, eui::Ui& ui) {
    using InputModel = components::input_detail::InputModel;
    const InputModel::InputState& inputState = ui.state<InputModel::InputState>(editorInputId(state));
    const LpPlan& plan = lp::cachedPlan(state.doc.text);
    const int line = plan.lineIndexFor(inputState.cursor);
    if (line < 0 || line >= static_cast<int>(plan.lines.size()) ||
        plan.lines[static_cast<std::size_t>(line)].kind != LpKind::Heading) {
        return 0;
    }
    return plan.lines[static_cast<std::size_t>(line)].headingLevel;
}

inline void dispatchEditorContextItem(AppState& state, const std::vector<int>& path,
                                      bool onLink, eui::Ui& ui) {
    if (path.empty()) {
        return;
    }
    std::vector<int> shifted = path;
    if (onLink) {
        const LinkTarget link = linkTargetUnderCursor(state, ui);
        if (shifted[0] == 0) {
            if (link.valid) {
                std::string docDir;
                if (!state.path.empty()) {
                    docDir = textfile::pathToUtf8(textfile::pathFromUtf8(state.path).parent_path());
                }
                openLinkTarget(state, link.url, docDir);
            }
            return;
        }
        if (shifted[0] == 1) {
            if (link.valid) {
                state.pendingLinkBeg = link.urlBeg;
                state.pendingLinkEnd = link.urlEnd;
                state.linkEditorUrl = link.url;
                state.linkEditorOpen = true;
            }
            return;
        }
        shifted[0] -= 2;
    }
    switch (shifted[0]) {
        case 0:
            state.pendingEditorCommand = EditorCommand::InsertBlock;
            state.pendingBlockKind = 5;
            return;
        case 1:
            state.pendingEditorCommand = EditorCommand::InsertBlock;
            state.pendingBlockKind = 6;
            return;
        case 2:
            if (shifted.size() == 2) {
                state.pendingEditorCommand = EditorCommand::FormatInline;
                state.pendingFormatKind = shifted[1] + 1;
            }
            return;
        case 3:
            if (shifted.size() == 2) {
                state.pendingEditorCommand = EditorCommand::SetHeading;
                state.pendingHeadingLevel = shifted[1];
            }
            return;
        case 4:
            if (shifted.size() == 2) {
                if (shifted[1] == 4) {
                    // 图片…（2026-10-06）：置位后由每帧 tick 弹文件对话框（同图片
                    // 粘贴的 IO 纪律），不走 InsertBlock 命令。
                    state.pendingImageImport = true;
                    app::requestUpdate();
                    return;
                }
                state.pendingEditorCommand = EditorCommand::InsertBlock;
                state.pendingBlockKind = shifted[1] + 1;
            }
            return;
        case 5:
            state.pendingEditorCommand = EditorCommand::Cut;
            return;
        case 6:
            state.pendingEditorCommand = EditorCommand::Copy;
            return;
        case 7:
            state.pendingEditorCommand = EditorCommand::Paste;
            return;
        case 8:
            state.pendingEditorCommand = EditorCommand::SelectAll;
            return;
        default:
            return;
    }
}

} // namespace context_menu_detail

// 与其它全屏弹层一起在 compose 末尾声明（app.cpp）：菜单本体按
// state.contextMenuX/Y（窗口逻辑坐标，组件回调给的）摆放并 clamp 到屏内。
inline void editorContextMenuOverlay(eui::Ui& ui, AppState& state, const eui::Screen& screen) {
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);
    components::theme::ThemeColorTokens menuTokens = colors.tokens;
    menuTokens.metrics.typography.option = metrics.menuFontSize;

    // 光标落在链接上时菜单多出"打开链接/编辑链接"两项（2026-09-26）。选区内右键
    // 保持插入点不动，是否加链接项严格依据 InputState 保存的实际右键命中位置。
    const bool onLink = context_menu_detail::linkTargetUnderCursor(state, ui).valid;

    components::contextMenu(ui, "editor.contextmenu")
        .open(state.contextMenuOpen)
        .screen(screen.width, screen.height)
        .position(state.contextMenuX, state.contextMenuY)
        .size(metrics.menuWidth, metrics.menuItemHeight)
        .items(context_menu_detail::editorContextMenuItems(
            onLink, context_menu_detail::editorHasSelection(ui),
            context_menu_detail::currentHeadingLevel(state, ui)))
        .theme(menuTokens)
        .transition(quickTransition())
        .zIndex(1100)
        .onSelectPath([&state, onLink, &ui](const std::vector<int>& path) {
            context_menu_detail::dispatchEditorContextItem(state, path, onLink, ui);
        })
        .onOutsideContextMenu([&state](float x, float y) {
            // 菜单开着时再次右键：刷新菜单位置（不关闭、不重置级联以外的状态）。
            // 落点换算已由编辑区的 onContextMenu 做过一次，这里的坐标是"菜单外"
            // 的原始落点 —— 直接当新的锚点用（组件内部会 clamp 到屏内）。
            state.contextMenuX = x;
            state.contextMenuY = y;
            app::requestUpdate();
        })
        .onOpenChange([&state](bool open) {
            // 点空白/选中某项后组件回调 close；这里同步应用层状态。
            if (!open) {
                state.contextMenuOpen = false;
            }
        })
        .build();
}

} // namespace neo
