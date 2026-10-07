#pragma once

#include "model/i18n.h"

#include "model/link_target.h"
#include "model/lp_decorations.h"
#include "model/text_file.h"
#include "platform/clipboard_image.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "state/tabs_trace.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

namespace neo {

// 供 InputBuilder::lineDecorator 使用的入口（T10 拆层：应用编排在 UI 层）。
// 光标由调用方传入——先从输入组件的状态里取出来再交进来，
// 避免在组件 build() 的过程中回头去查同一个 ui.state。
//
// 这里收 AppState、读完 markdownCapable/path/theme/foldedHeadings 之后，
// 全部转成 model 层认识的原始参数（字号、字体、主题枚举、折叠集合、docDir），
// 再调 lp::cachedPlan / lp::cachedDecorations —— model 层不 include app_state.h。
inline components::input_detail::LineDecorationSnapshot decorationsForEditor(
    const AppState& state, const std::string& text, int cursor,
    const components::input_detail::DecoratorEditInfo& editInfo) {
    static const auto empty = std::make_shared<const components::input_detail::LineDecorationTable>();
    if (!state.markdownCapable()) {
        if (state.sourceView()) return source::snapshot(text, state.language(), editorColors(state.theme), themeRevision()*2 + (state.theme==ThemeMode::Light?1:0), editInfo);
        return empty;
    }
    const EditorColors& colors = editorColors(state.theme);
    const components::MarkdownStyle style =
        markdownStyle(state.editorFontSize, editorFontFamily(state), codeFontFamily(state), colors,
                      state.theme);
    // 编辑区间随上下文递给 model 层（T4 A2 / T5 局部重解析）：链不完整（IME 合成、
    // 换文档、两次编辑之间没建过表…）时 lp 层自己退回全量，这里不做判断也不缓存
    // 任何编辑状态 —— 折叠键（foldedHeadings）同样按"交给 model 层原样判定"处理。
    const LpPlan& plan = [&]() -> const LpPlan& {
        tracelog::Span span("lp-plan");
        return lp::cachedPlan(text, &editInfo);
    }();
    // 相对图片路径的基准 = 文档所在目录（未保存的新文档 path 为空 → cwd，不解析相对图）。
    std::string docDir;
    if (!state.path.empty()) {
        docDir = textfile::pathToUtf8(textfile::pathFromUtf8(state.path).parent_path());
    }
    // 编辑区间随上下文递给 model 层（T4 A2）：链不完整（IME 合成、换文档、两次编辑
    // 之间没建过表…）时 lp 层自己退回全量，这里不做判断也不缓存任何编辑状态 ——
    // 折叠键（foldedHeadings）同样按"交给 model 层原样判定"处理，UI 层不做静态平移。
    // 主题文件版本（T13 收尾）由**这里显式递进去**：ThemeMode 只有深浅两档，换主题
    // 文件时枚举不变、字号字体也都不动 —— 不把 themeRevision() 递进装饰缓存键，
    // 同外观换主题文件就会命中上一套主题的表（标题/代码/引用继续画旧色）。
    const auto table = [&] {
        tracelog::Span span("lp-decorations");
        return lp::cachedDecorationSnapshot(
            plan, lp::planCache().version, cursor, style, editorFontFamily(state), state.theme, docDir,
            &state.foldedHeadings, text, &colors, &editInfo, themeRevision());
    }();
    lp::debugLogDecorations(cursor, plan.lineIndexFor(cursor), *table);
    return table;
}

// 编辑区是这个应用唯一的文档视图。
//
// - `.md` 走 Live Preview：标记就地隐藏、按块级字号排版（标题更大、代码块用等宽小一号），
//   光标所在块显示原始源码；
// - 其它文件走无格式的纯文本编辑 —— 与 Obsidian 一致，纯文本本来就没有格式可渲染。
//
// 原先独立的"预览"视图已按 2026-09-23 的决定移除（做坏了直接改，做好了也没必要留两份）。
inline void editorView(eui::Ui& ui, AppState& state, const UiMetrics& metrics, float width, float height) {
    const EditorColors& colors = editorColors();
    if (state.unavailable) {
        ui.column("editor.unavailable").size(width, height).padding(24.0f).gap(12.0f).content([&] {
            ui.text("editor.unavailable.title").size(std::max(0.0f, width - 48.0f), 40.0f)
                .text(i18n::tr("safety.open_failed")).fontFamily(uiFontFamily(state))
                .fontSize(metrics.editorFontSize).color(colors.text).wrap(true).build();
            ui.text("editor.unavailable.error").size(std::max(0.0f, width - 48.0f), 90.0f)
                .text(state.path + "\n" + state.loadError).fontFamily(uiFontFamily(state))
                .fontSize(metrics.menuFontSize).color(colors.textMuted).wrap(true).build();
            iconTextButton(ui, "editor.unavailable.retry", UiIcon::ArrowLeft, i18n::tr("tabs.retry"), 130.0f,
                metrics.menuFontSize, [&state] { retryUnavailableDocument(state); }, uiFontFamily(state));
        }).build();
        return;
    }


    components::InputStyle style(colors.tokens);
    style.background = colors.editor;
    style.focused = colors.editor;
    style.border = transparentColor();
    style.focusBorder = transparentColor();
    style.text = colors.text;
    style.placeholder = colors.textMuted;
    style.cursor = colors.accent;
    style.shadow = eui::Shadow{};
    style.radius = 0.0f;

    using InputModel = components::input_detail::InputModel;
    // 光标存在输入组件自己的 state 里，这里先取一次再交给 provider；
    // 在组件 build() 的过程中回头去查同一个 state 容易踩到"构建中访问"的坑。
    //
    // 对齐 Obsidian（用户反馈 2026-09-24）：鼠标按住拖选期间，"光标附近标记显隐"
    // 冻结在**按下前**的光标上，松手后才按实时光标判断 —— 否则正常拖选范围时
    // 活动块一路切换、布局跟着变形。键盘移动光标不受影响（selecting 只在按住时为真）。
    const InputModel::InputState& inputState = ui.state<InputModel::InputState>(editorInputId(state));
    const int cursor = inputState.selecting ? inputState.cursorBeforePress : inputState.cursor;

    // ── R8：折叠状态的唯一更新入口（每帧一次，都在 input build() 之前）──────────
    // 1) 与编辑器文本代次对账：文本变过就把折叠键平移，证明不了就清空展开 ——
    //    打字 / 粘贴 / 剪切 / 命令回写 / undo-redo / CleanAi 走的是同一个落点；
    // 2) 消费排队中的"查找命中展开"：plan 那一帧就绪，这一帧就生效。
    {
        auto& mutableInput =
            ui.state<components::input_detail::InputModel::InputState>(editorInputId(state));
        syncFoldsWithEditorText(state, mutableInput);
        if (applyPendingFindReveal(state, mutableInput)) {
            app::requestUpdate();  // plan 还没就绪：再要一帧来消费排队中的展开
        }
    }

    // ── 折叠命令（S3f 批次 C）──────────────────────────────────────────────
    // applyEditorCommand（app_state，看不见 lp_plan）对这两个命令直通放行，compose
    // 走到这里才执行 —— 此时 plan 缓存可用，且在 input build() 之前，同一帧就能生效。
    if (state.pendingEditorCommand == EditorCommand::ToggleFold ||
        state.pendingEditorCommand == EditorCommand::UnfoldAt) {
        const EditorCommand foldCommand = state.pendingEditorCommand;
        state.pendingEditorCommand = EditorCommand::None;
        const LpPlan& plan = lp::cachedPlan(state.doc.text);
        const int headingBeg = plan.foldHeadingBegFor(inputState.cursor);
        bool changed = false;
        if (headingBeg >= 0) {
            if (foldCommand == EditorCommand::UnfoldAt) {
                changed = state.foldedHeadings.erase(headingBeg) > 0;
            } else if (state.foldedHeadings.count(headingBeg) > 0) {
                changed = state.foldedHeadings.erase(headingBeg) > 0;
            } else {
                // 不变量"光标永不在折叠行上"：光标在章节**体内**时拒绝折叠 ——
                // 允许的位置只有标题行本身（setext 的下划线行也算标题配对）。
                const int cursorLine = plan.lineIndexFor(inputState.cursor);
                const int headingLine = plan.lineIndexFor(headingBeg);
                const LpLine& heading = plan.lines[static_cast<std::size_t>(headingLine)];
                const int pairEnd = headingLine + (heading.setext ? 1 : 0);
                if (cursorLine <= pairEnd) {
                    state.foldedHeadings.insert(headingBeg);
                    changed = true;
                }
            }
        }
        if (changed) {
            // 折叠状态进装饰缓存键（lp 层 foldKey）之外，还要推进 decorationRevision：
            // 根 dirtyKey 含它，整棵子树才会按新装饰重建（同"字重合"那一课）。
            ++ui.state<InputModel::InputState>(editorInputId(state)).decorationRevision;
        }
        if (const char* dbg = std::getenv("NEO_LP_DEBUG")) {
            if (std::FILE* fileHandle = std::fopen(dbg, "a")) {
                std::fprintf(fileHandle,
                             "fold: cmd=%d headingBeg=%d cursorLine=%d changed=%d set=%zu\n",
                             static_cast<int>(foldCommand), headingBeg,
                             plan.lineIndexFor(inputState.cursor),
                             changed ? 1 : 0, state.foldedHeadings.size());
                std::fclose(fileHandle);
            }
        }
    }

    const auto statsBefore = InputModel::debugLayoutStats();
    components::input(ui, editorInputId(state))
        .size(width, height)
        .multiline(true)
        .wordWrap(state.wordWrap())
        .viewportMetrics(std::getenv("NEO_VIEWPORT_METRICS_OFF") == nullptr)
        .scrollbar(true)
        // 光标闪烁（组件默认关闭，只有编辑器需要）。
        .caretBlink(true)
        .inset(metrics.editorInset)
        // 限制行宽（Obsidian 的"可读行宽"）：内容列收窄到 43.75em（700px @ 16px）
        // 并居中，行号列与滚动条仍贴两缘。关闭时传 0 = 不限制。
        .maxTextWidth(state.readableWidth && !state.sourceView() && state.wordWrap() ? metrics.editorFontSize * 43.75f : 0.0f)
        // 行号列（画在编辑器左缘留白 + 专属槽位里，随主题的次要文字色）。
        .lineNumbers(state.showLineNumbers)
        .lineNumberColor(colors.textMuted)
        .fontSize(metrics.editorFontSize)
        // 自选字体 > 系统正文字体，见 editorFontFamily()。
        .fontFamily(state.sourceView() ? codeFontFamily(state) : editorFontFamily(state))
        .style(style)
        .transition(quickTransition())
        .value(state.doc.text)
        .placeholder(i18n::tr("editor.placeholder"))
        // Live Preview：逐行的字号/行高、要隐藏的标记、活动块，全部由 lp 适配层算。
        .lineDecorationSnapshot([&state, cursor](const std::string& text,
                                        const components::input_detail::DecoratorEditInfo& info) {
            return decorationsForEditor(state, text, cursor, info);
        })
        .onChange([&state](const std::string& value) {
            if (state.sessionClosePending || state.closeApproved) return;
            if (value == state.doc.text) {
                return;
            }
            state.doc.text = value;
            ++state.revision;
            state.recovered = false;
            maybeWriteRecovery(state);
        })
        // 图片粘贴（R2）：Ctrl+V 且剪贴板无文本、有位图时转附件流程。只在编辑器
        // 实例挂回调——查找栏/弹窗输入不挂，纯文本粘贴行为原样不变。
        // 剪贴板里既无文本、位图也无单个受支持图片文件：明确拒绝，不静默吞掉。
        .onImagePaste([&state] {
            if (!state.markdownCapable()) {
                showToast(state, i18n::tr("safety.cannot_insert_image"),
                          i18n::tr("safety.image_markdown_only"));
            } else if (neo::clipboardimage::available()) {
                state.pendingImagePaste = true;
                app::requestUpdate();
            } else {
                showToast(state, i18n::tr("safety.paste_rejected"),
                          i18n::tr("safety.paste_no_image"));
            }
        })
        // 命中回调（S3f 批次 A/E）。四个分支，互不叠加：
        //   onImage  → 点块级图片：开全屏预览（组件已跳过移光标，图不会被切成源码）；
        //   onGutter → 点行号列：落在标题起始行上 = 折叠/展开该章节（C 的收尾交互；
        //              其余行只移光标，是组件的默认行为，这里不再排队命令）；
        //   onLink   → 点链接（2026-09-26）：单击跳转 —— 外部 URL 交系统，本地路径
        //              按文档目录/文档库根解析打开；
        //   onGlyph  → 任务复选框翻转（决策⑤）。
        // 事件阶段只做只读判定 + 排队/置状态，真正改文档与重画都在之后的 compose。
        .onPointerHit([&state](const InputModel::PointerHit& hit) {
            const LpPlan& plan = lp::cachedPlan(state.doc.text);
            if (hit.onImage) {
                // 组件在 onImage 上不移光标，byteIndex 是洞边界的最近停靠点（行首/行尾），
                // 用源行号回查 plan 才靠得住（排版行号 ≠ 源行号）。
                const std::size_t sourceLine = static_cast<std::size_t>(std::max(1, hit.lineNumber) - 1);
                if (sourceLine >= plan.lines.size()) {
                    return;
                }
                const LpLine& line = plan.lines[sourceLine];
                if (line.pureImageSrc.empty()) {
                    return;
                }
                std::string docDir;
                if (!state.path.empty()) {
                    docDir = textfile::pathToUtf8(textfile::pathFromUtf8(state.path).parent_path());
                }
                const std::string resolved = lp::resolveLocalImageSrc(line.pureImageSrc, docDir);
                std::optional<lp::ImageExtent> extent;
                if (!resolved.empty()) {
                    extent = lp::readImageExtent(resolved);
                }
                if (!resolved.empty() && extent.has_value()) {
                    state.imagePreviewPath = resolved;
                    state.imageViewport = ImageViewport{};
                    state.imagePreviewWidth = static_cast<float>(extent->width);
                    state.imagePreviewHeight = static_cast<float>(extent->height);
                    app::requestUpdate();
                }
                return;
            }
            if (hit.onGutter) {
                // 点击行号列：只有"标题起始行"（ATX 或 setext 配对的文本行）才切折叠。
                // foldHeadingBegFor(行首字节) 对标题行返回它自己、对体内行返回包住它的
                // 标题 —— 起始行号相等才认。onPress 已把光标移到该行，ToggleFold 在
                // compose 里按光标算出的目标与这里一致。
                const std::size_t sourceLine = static_cast<std::size_t>(std::max(1, hit.lineNumber) - 1);
                if (sourceLine < plan.lines.size()) {
                    const int headingBeg = plan.foldHeadingBegFor(hit.byteIndex);
                    const int headingLine = headingBeg >= 0 ? plan.lineIndexFor(headingBeg) : -1;
                    if (headingLine == static_cast<int>(sourceLine) &&
                        state.pendingEditorCommand == EditorCommand::None) {
                        state.pendingEditorCommand = EditorCommand::ToggleFold;
                    }
                }
                return;
            }
            if (hit.onLink) {
                // 链接跳转：目标在事件阶段解析（可能弹确认对话框 / 打浏览器），
                // 打开失败走 toast。光标已被组件移进链接片段（标记露出源码），
                // 与 Obsidian 的行为一致 —— 想改链接就右键。
                std::string docDir;
                if (!state.path.empty()) {
                    docDir = textfile::pathToUtf8(textfile::pathFromUtf8(state.path).parent_path());
                }
                const LinkTarget target = linkTargetAt(plan, state.doc.text, hit.byteIndex);
                if (target.valid) {
                    openLinkTarget(state, target.url, docDir);
                }
                return;
            }
            if (!hit.onGlyph) {
                return;
            }
            const int taskByte = plan.taskStateByteFor(hit.byteIndex);
            if (taskByte < 0) {
                return;
            }
            state.pendingEditorCommand = EditorCommand::ToggleTask;
            state.pendingTaskByte = taskByte;
        })
        // 光标落进折叠隐藏行（↓ 走到折叠段、撤销恢复到折叠区）→ 排队展开。
        // 组件只在该行真的 hidden 时每帧回调一次，幂等；不覆盖已排队的其它命令。
        .onRevealHiddenLine([&state](int) {
            if (state.pendingEditorCommand == EditorCommand::None) {
                state.pendingEditorCommand = EditorCommand::UnfoldAt;
            }
        })
        // 右键（2026-09-25）：组件已按"选区内保留选区、选区外移光标"处理落点，
        // 这里只记下弹层位置并打开。菜单本体在 editorContextMenuOverlay（app.cpp）。
        .onContextMenu([&state](float x, float y) {
            state.contextMenuX = x;
            state.contextMenuY = y;
            state.contextMenuOpen = true;
            app::requestUpdate();
        })
        .build();
    if (tracelog::enabled()) {
        const auto& stats = InputModel::debugLayoutStats();
        const auto& input = ui.state<InputModel::InputState>(editorInputId(state));
        tracelog::event("layout-work", 0,
            "tab=" + std::to_string(state.tabId) + " revision=" + std::to_string(input.textRevision) +
            " full=" + std::to_string(stats.full - statsBefore.full) +
            " coarse=" + std::to_string(stats.coarseRows - statsBefore.coarseRows) +
            " detailed=" + std::to_string(stats.detailMeasuredRows - statsBefore.detailMeasuredRows) +
            " resident=" + std::to_string(input.detailedRows.size()) +
            " rows=" + std::to_string(input.cachedLines.size()));
    }
}

} // namespace neo
