#pragma once

#include "components/theme.h"
#include "components/input_model.h"
#include "components/scroll.h"
#include "components/vector_icon.h"
#include "core/dsl.h"
#include "eui/signal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace components {

struct InputStyle {
    InputStyle() : InputStyle(theme::dark()) {}

    explicit InputStyle(const theme::ThemeColorTokens& tokens) {
        background = tokens.surface;
        focused = theme::resolveFieldFill(tokens, tokens.surface, 0.20f, 0.70f);
        border = theme::withOpacity(tokens.border, 0.78f);
        focusBorder = theme::withAlpha(tokens.primary, 0.86f);
        text = tokens.text;
        placeholder = theme::withOpacity(tokens.text, 0.45f);
        cursor = tokens.primary;
        shadow = theme::popupShadow(tokens);
        radius = tokens.metrics.radius.popup;
    }

    core::Color background;
    core::Color focused;
    core::Color border;
    core::Color focusBorder;
    core::Color text;
    core::Color placeholder;
    core::Color cursor;
    core::Shadow shadow;
    float radius = 10.0f;
};

class InputBuilder {
public:
    InputBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    InputBuilder& x(float value) { x_ = value; hasX_ = true; return *this; }
    InputBuilder& y(float value) { y_ = value; hasY_ = true; return *this; }
    InputBuilder& position(float xValue, float yValue) { return x(xValue).y(yValue); }
    InputBuilder& size(float width, float height) { width_ = width; height_ = height; return *this; }
    InputBuilder& value(std::string value) { text_ = std::move(value); return *this; }
    InputBuilder& bind(eui::Signal<std::string>& signal) {
        value(signal.get());
        onChange([&signal](const std::string& value) { signal.set(value); });
        return *this;
    }
    InputBuilder& placeholder(std::string value) { placeholder_ = std::move(value); return *this; }
    InputBuilder& multiline(bool value = true) { multiline_ = value; return *this; }
    InputBuilder& wordWrap(bool value = true) { wordWrap_ = value; return *this; }
    InputBuilder& viewportMetrics(bool value = true) { viewportMetrics_ = value; return *this; }
    /** @brief 多行输入溢出时显示垂直滚动条，默认关闭；不影响滚轮和光标跟随。 */
    InputBuilder& scrollbar(bool value = true) { scrollbar_ = value; return *this; }
    InputBuilder& fontSize(float value) { fontSize_ = std::max(1.0f, value); return *this; }
    InputBuilder& fontFamily(std::string value) { fontFamily_ = std::move(value); return *this; }
    InputBuilder& inset(float value) { inset_ = std::max(0.0f, value); return *this; }
    /**
     * @brief 内容列最大宽度（"限制行宽"/可读行宽，仅多行时生效，<= 0 关闭）。
     *
     * 可用宽度超过它时，左右内缩各加一半的超出量把内容列收窄到 maxTextWidth 并居中；
     * 行号列与滚动条仍贴控件两缘，块级底色/竖条跟内容列走（它们以文本原点定位）。
     */
    InputBuilder& maxTextWidth(float value) { maxTextWidth_ = std::max(0.0f, value); return *this; }
    /** @brief 行号列（仅多行时生效）。开启后文本区左侧留出一列右对齐的行号。 */
    InputBuilder& lineNumbers(bool value = true) { lineNumbers_ = value; return *this; }
    /** @brief 行号文字色。不设置时用 placeholder 色（随主题的"次要文字"档）。 */
    InputBuilder& lineNumberColor(core::Color value) { lineNumberColor_ = value; return *this; }
    /** @brief 悬停光标，默认文本输入应有的 IBeam；传 Hand/Arrow 可覆盖。 */
    InputBuilder& cursor(core::CursorShape value) { cursor_ = value; return *this; }
    InputBuilder& style(const InputStyle& value) { style_ = value; return *this; }
    InputBuilder& theme(const theme::ThemeColorTokens& tokens) {
        style_ = InputStyle(tokens);
        scrollStyle_ = ScrollStyle(tokens);
        metrics_ = tokens.metrics;
        return *this;
    }
    InputBuilder& transition(const core::Transition& value) { transition_ = value; return *this; }
    InputBuilder& transition(float duration, core::Ease ease = core::Ease::OutCubic) {
        transition_ = core::Transition::make(duration, ease);
        return *this;
    }
    /**
     * @brief 光标闪烁，默认关闭。
     *
     * 开启后组件在聚焦期间挂一个 0.53s 的定时器（不是逐帧回调）来翻转光标可见性；
     * 输入 / 移动光标 / 改选区时会立刻恢复常亮，之后才继续闪 —— 与常见编辑器一致。
     * 用定时器而不是逐帧：闪烁不需要 60fps，逐帧回调会让应用满速空转（实测高两个数量级）。
     */
    InputBuilder& caretBlink(bool value = true) { caretBlink_ = value; return *this; }
    InputBuilder& onChange(std::function<void(const std::string&)> callback) {
        onChange_ = std::move(callback);
        return *this;
    }
    InputBuilder& onImagePaste(std::function<void()> callback) {
        onImagePaste_ = std::move(callback);
        return *this;
    }
    InputBuilder& onEnter(std::function<void()> callback) {
        onEnter_ = std::move(callback);
        return *this;
    }
    InputBuilder& onEscape(std::function<void()> callback) {
        onEscape_ = std::move(callback);
        return *this;
    }
    InputBuilder& onFocus(std::function<void(bool)> callback) {
        onFocus_ = std::move(callback);
        return *this;
    }
    /**
     * @brief 逐行装饰（Live Preview 用）。**不设置时组件完全退回原来的纯文本行为。**
     *
     * provider 接收当前文本与光标位置，返回"每个物理行一项"的装饰表：该行的字号、行高，
     * 以及要从该行里隐藏掉的字节区间（会以"投影文本"的形式参与测量、光标、命中）。
     * 组件本身不认识 markdown——样式与隐藏策略全部由上层决定。
     *
     * 这是**只收 text 的旧签名**：包装成"不带编辑区间"的上下文，调用方行为一字不变
     * （T4 A2 的向后兼容重载；不认识增量的使用者无需改代码）。
     */
    InputBuilder& lineDecorator(input_detail::LineDecorationProvider provider) {
        snapshotDecorator_ = {};
        decorator_ = [provider](const std::string& text, const input_detail::DecoratorEditInfo&) {
            return provider(text);
        };
        return *this;
    }
    /**
     * @brief 逐行装饰（带编辑上下文，T4 A2）。
     *
     * 回调额外拿到 DecoratorEditInfo：本次 text 的 textRevision、它是不是公共漏斗
     * 产出的文档正文（IME 合成中的临时文本 committed == false）、以及这次改动的
     * 编辑区间（byteBeg/oldEnd/newEnd + 受影响的源行号）。装饰层据此把"区间外整行
     * 复用 + 平移"与"区间内重算"分开做；拿不到可信区间时自己退回全量即可。
     */
    InputBuilder& lineDecorator(input_detail::LineDecorationProviderEx provider) {
        snapshotDecorator_ = {};
        decorator_ = std::move(provider);
        return *this;
    }
    InputBuilder& lineDecorationSnapshot(input_detail::LineDecorationSnapshotProvider provider) {
        decorator_ = {};
        snapshotDecorator_ = std::move(provider);
        return *this;
    }
    /**
     * @brief 指针语义动作回调（opt-in，**不设置即零变化**）。
     *
     * 图元与链接在有效松开时触发，拖出取消；正文、行号和图片保留按下回调。
     * 命中字节与 cursorFromPointer 同源，拖动/移动不重复执行动作。
     */
    InputBuilder& onPointerHit(std::function<void(const input_detail::InputModel::PointerHit&)> callback) {
        onPointerHit_ = std::move(callback);
        return *this;
    }
    /**
     * @brief 光标落进折叠隐藏行时回调（S3f 批次 C，opt-in，不设置即零变化）。
     *
     * 组件自己不认识"折叠"：它只知道"这一行 hidden 而光标在上面"（典型路径 ↓ 走到
     * 折叠段、撤销恢复到折叠区）。把它抛给应用层去展开对应章节；组件每帧检查一次，
     * 直到该行不再 hidden（展开是应用层的事，组件不改任何文档状态）。
     */
    InputBuilder& onRevealHiddenLine(std::function<void(int)> callback) {
        onRevealHiddenLine_ = std::move(callback);
        return *this;
    }
    /**
     * @brief 右键菜单回调（opt-in，不设置即零变化）。
     *
     * 组件先做与左键一致的落点处理（点在已有选区内 → 选区原样保留，菜单命令才能
     * 作用于选区；点在选区外 → 光标移到落点并清空选区），再把弹层的建议位置
     * （窗口逻辑坐标，与 position() 同一坐标系）抛给应用层 —— 开不开菜单由应用层定。
     */
    InputBuilder& onContextMenu(std::function<void(float x, float y)> callback) {
        onContextMenu_ = std::move(callback);
        return *this;
    }

    void build() {
        const std::string hitId = id_ + ".hit";
        const bool focused = ui_.isFocused(hitId);
        const float baseInset = inset_ >= 0.0f ? inset_ : metrics_.spacing.content;
        const float fontSize = fontSize_ > 0.0f ? fontSize_ : metrics_.typography.input;
        // 行号列宽：按最宽行号（位数 × "8" 的实测宽）+ 右侧 8px 间距。
        // measureTextWidth 走核心层的 shaping 缓存，同字号第一帧之后就是查表。
        float gutterWidth = 0.0f;
        if (lineNumbers_ && multiline_) {
            int totalLines = 1;
            for (const char c : text_) {
                totalLines += c == '\n' ? 1 : 0;
            }
            const int digits = std::max(2, static_cast<int>(std::to_string(totalLines).size()));
            const float digitsWidth = core::TextPrimitive::measureTextWidth(
                std::string(static_cast<std::size_t>(digits), '8'), fontFamily_, fontSize);
            gutterWidth = digitsWidth + 8.0f;
        }
        // 语义约定：此后 `inset` 一律指**左缘内缩**（含行号列），`rightInset` 指**右缘
        // 内缩**。所有"到文本原点的距离"（命中换算、光标、选区裁剪）用 inset；
        // 所有"对称宽度"（换行宽、横向滚动范围）用 inset + rightInset。
        float inset = baseInset + gutterWidth;
        float rightInset = baseInset;
        // 开启时固定预留槽位，避免溢出临界点因滚动条显隐反复改变换行宽度。
        // 右侧留白是 baseInset（行号列只在左边），别把 gutterWidth 也算进来，
        // 否则开行号时滚动条整体左移一段。
        const float scrollbarWidth = scrollbar_ && multiline_
            ? std::min(metrics_.control.scrollbar, std::max(0.0f, width_ - baseInset * 2.0f)) : 0.0f;
        const float scrollbarGutter = scrollbarWidth > 0.0f ? scrollbarWidth + 4.0f : 0.0f;
        float textWidth = std::max(0.0f, width_ - inset - rightInset - scrollbarGutter);
        // 限制行宽（多行）：内容列收窄到 maxTextWidth 并居中。只动左右内缩——行号列
        // 仍贴左缘、滚动条仍贴右缘（barX 用控件右缘），块级底色/竖条以文本原点定位、
        // 自动跟内容列走。垂直几何（textY/textHeight）是**独立**的，不受左右收窄影响。
        if (maxTextWidth_ > 0.0f && multiline_ && textWidth > maxTextWidth_) {
            const float extra = (textWidth - maxTextWidth_) * 0.5f;
            inset += extra;
            rightInset += extra;
            textWidth = maxTextWidth_;
        }
        const bool allowMultiline = multiline_;
        const std::function<void(const std::string&)> onChange = onChange_;
        const std::function<void()> onImagePaste = onImagePaste_;
        const std::function<void()> onEnter = onEnter_;
        const std::function<void()> onEscape = onEscape_;
        const std::function<void(bool)> onFocus = onFocus_;
        // 拷到局部再进 lambda：直接在捕获初始化里读成员会隐式要求捕获 this。
        const std::function<void(const InputModel::PointerHit&)> onPointerHit = onPointerHit_;
        const std::function<void(float, float)> onContextMenu = onContextMenu_;
        const float textLineHeight = fontSize * 1.2f;
        // ── 纵向留白（2026-09-27 实测修正）──────────────────────────────────────
        // 两条实测事实（Release + 125% 系统缩放，文档 = 一行一个 URL 的 .txt）：
        //   ① 行距 = 字号 × 1.2（16px → 24 物理 px，实测吻合）；
        //   ② 文本图元的**墨迹会探出行盒顶**约 0.1em（16px ≈ 2 物理 px）。
        // 第 ② 条是上一轮"纵向内缩归零"后暴露的：文本视口的裁剪框与行盒顶重合，
        // 首行的上升部被裁掉那一截，看着就是"文字和上下边缘重叠 2-3px"。那次把内缩
        // 设成 0 是**推理**出来的（只想到 gutterWidth 是水平量、不该减在高度上），
        // 没有实测，所以漏了这条墨迹溢出。
        //
        // 修法：纵向留一点显式呼吸，并让**裁剪框比文本行盒上探 inkOverhang** ——
        // 首行墨迹因此完整、不被裁。行盒顶 textY 比裁剪框顶低 inkOverhang，
        // 这段落差靠 textViewport 的 padding 平移子元素实现（见那里的注释）。
        //
        // 单行视口不额外加 padding：它从居中的名义行盒开始；文字图元在行盒内按墨迹居中。
        const float inkOverhang = fontSize * 0.10f;
        // 单行输入按行盒在控件内居中，墨迹使用 TextPrimitive 的 VerticalAlign::Center
        // 按当前字体真实 glyph inkTop/inkBottom 居中。不要用固定 em 偏移猜 baseline：
        // 字体回退、字号取整与 DPI hinting 都会改变墨迹相对行盒的位置。
        const float centeredTextY = std::max(0.0f, (height_ - textLineHeight) * 0.5f);
        const float textViewportTop = multiline_ ? inkOverhang + 2.0f : centeredTextY;
        const float textViewportPadTop = multiline_ ? inkOverhang : 0.0f;
        const float textY = multiline_ ? textViewportTop + textViewportPadTop : centeredTextY;
        // 文本可视区高度 = 控件高 − 上下留白。行号列（左）/ 滚动条（右）占的是**水平**
        // 空间，已由 textWidth 里的 inset(+gutterWidth) 与 scrollbarGutter 让开，
        // 不在高度上再减（原式减 gutterWidth 是维度搞混）。单行 = 行高。
        const float textHeight = multiline_
            ? std::max(0.0f, height_ - textViewportTop * 2.0f - (!wordWrap_ ? 12.0f : 0.0f))
            : textLineHeight;
        const float width = width_ - scrollbarGutter;
        const float controlWidth = width_;
        const std::string fontFamily = fontFamily_;
        InputState& state = ui_.state<InputState>(id_);
        state.wordWrap = wordWrap_;
        state.viewportMetrics = viewportMetrics_;
        if (state.text != text_) {
            const bool wasFocused = focused;
            state.text = text_;
            ++state.textRevision;
            // 组件外的直接赋值（换文档 / 应用层回写）没有可信的前后对照：
            // 编辑区间作废，装饰与布局一律全量重建（T4 A1）。
            InputModel::clearPendingEdit(state);
            state.cursor = InputModel::clampUtf8Boundary(state.text, static_cast<int>(state.text.size()));
            state.selectionStart = state.cursor;
            state.selectionEnd = state.cursor;
            if (!wasFocused) {
                state.horizontalScroll = 0.0f;
                state.verticalScroll = 0.0f;
                state.undoStack.clear();
                state.redoStack.clear();
            }
        }
        state.cursor = InputModel::clampUtf8Boundary(state.text, state.cursor);
        state.selectionStart = InputModel::clampUtf8Boundary(state.text, state.selectionStart);
        state.selectionEnd = InputModel::clampUtf8Boundary(state.text, state.selectionEnd);
        if (caretBlink_) {
            // 有活动（输入 / 移动光标 / 改选区）时立刻可见，并让紧接着那一拍不翻转。
            InputModel::caretBlinkObserve(state);
        }
        const bool hasComposition = focused && !state.compositionText.empty();
        InputState& display = InputModel::displayState(state, hasComposition);
        // 装饰先于排版确定：它决定每行的字号/行高，以及要从行里隐藏掉哪些字节。
        // provider 是"文本 + 当前块"的纯函数（活动块由它自己按光标算），组件只比较内容——
        // 光标在同一块内移动时这里返回的表逐项相同，于是一次测量都不会做。
        //
        // 编辑上下文（T4 A2）：revision 取**文档**（真身 state）的 textRevision；合成中
        // 交给 provider 的是 displayState 的临时文本，committed 置 false 让它退回全量；
        // pendingEdit 只在"它描述的就是当前这份文本"（revision 对得上）时才递出去。
        input_detail::DecoratorEditInfo editInfo;
        input_detail::LineDecorationChanges decorationChanges;
        editInfo.changes = &decorationChanges;
        editInfo.textRevision = state.textRevision;
        editInfo.committed = !hasComposition;
        editInfo.edit = (editInfo.committed && state.pendingEdit.valid &&
                         state.pendingEdit.revision == state.textRevision)
                            ? &state.pendingEdit
                            : nullptr;
        const std::vector<input_detail::LineDecoration> decorations =
            decorator_ ? decorator_(display.text, editInfo)
                       : std::vector<input_detail::LineDecoration>{};
        const auto snapshot = snapshotDecorator_ ? snapshotDecorator_(display.text, editInfo)
                                                  : input_detail::LineDecorationSnapshot{};
        const input_detail::LineDecorationView table = snapshot ? input_detail::LineDecorationView(snapshot.get()) : input_detail::LineDecorationView(decorations.empty() ? nullptr : &decorations);
        // All pointer callbacks use the same frame's layout. In particular, a
        // long cursor line's metrics must not be deep-copied into each closure.
        // Keep the layout object alive after build() returns; its cache pointers
        // retain their existing InputState lifetime and are replaced next frame.
        const auto callbackLayout = std::make_shared<const InputLayout>(InputLayout::build(
            display, textWidth, textHeight, width, inset, rightInset, textY, textLineHeight,
            fontFamily_, fontSize, multiline_, table, snapshot, &decorationChanges));
        const InputLayout& layout = *callbackLayout;
        // 输入光标用字体字号的近似高度；将它放到单行行盒中心，使其与由实际墨迹范围
        // 居中的文字共享同一中心。多行仍采用布局给出的行内坐标。
        const float cursorFontSize = layout.cursorLineFontSize();
        const float caretHeight = (cursorFontSize > 0.0f ? cursorFontSize : fontSize) * 1.18f;
        const float caretY = multiline_
            ? layout.cursorY
            : textY + std::max(0.0f, (textLineHeight - caretHeight) * 0.5f);
        // 不变量"光标永不在折叠行上"的自愈点（S3f 批次 C）：走到这里说明光标停在
        // hidden 行上（撤销恢复、或 ↓ 落进折叠段的当帧）。回调只是**排队**让应用层
        // 展开——组件不碰文档状态；展开前的这一帧由装饰层的"光标行强制可见"兜住画面。
        // 检查看 hiddenByFold（折叠致隐原始值）：光标行被强制可见时 hidden 已放开，
        // 但"本该折叠"的事实还在 hiddenByFold 上——否则自愈永远不触发，折叠卡在半开。
        if (onRevealHiddenLine_ && !layout.lineList().empty()) {
            const int revealLine = layout.lineIndexFor(state.cursor);
            if (revealLine >= 0 &&
                revealLine < static_cast<int>(layout.lineList().size()) &&
                (layout.lineList()[static_cast<std::size_t>(revealLine)].hidden ||
                 layout.lineList()[static_cast<std::size_t>(revealLine)].hiddenByFold)) {
                onRevealHiddenLine_(state.cursor);
            }
        }
        state.horizontalScroll = display.horizontalScroll;
        state.verticalScroll = display.verticalScroll;
        const bool empty = display.text.empty();
        const bool hasSelection = !layout.selectionRects.empty();
        // 占位文案可在文本/布局不变时切换（语言或输入模式）。根子树与文字图元
        // 各有 retained key，两处都必须包含它；有内容时不让无关占位变化重建文档。
        const std::string placeholderKey = empty ? "|placeholder:" + placeholder_ : std::string{};
        // 注意 display.decorationRevision：框架把这个 key 当"内容签名"（key 相同就完全不更新
        // 图元文本），而光标移动只会改变装饰（活动块切换、标记显示与否）、不改 textRevision，
        // 所以装饰的版本号必须算进来，否则点了别处这一行仍画着旧文本。
        // textWidth / fontSize 也要进 key：窗口缩放、改字号只改几何输入，textRevision 不动；
        // 漏了它们框架会认为"文本没变"，把旧字号/旧宽度的字形直接搬到新位置 —— 偶发的
        // "个别行字重合"就是这么来的。
        const std::string textDirtyKey = id_ + ".text|" + std::to_string(state.textRevision) + "|" +
            std::to_string(display.textRevision) + "|" + std::to_string(display.decorationRevision) +
            "|" + std::to_string(static_cast<int>(std::lround(state.horizontalScroll * 64.0f))) +
            "|" + std::to_string(static_cast<int>(std::lround(state.verticalScroll * 64.0f))) +
            "|" + std::to_string(static_cast<int>(std::lround(textWidth * 64.0f))) +
            "|" + std::to_string(static_cast<int>(std::lround(fontSize * 64.0f))) +
            (wordWrap_ ? "|wrap" : "|nowrap") +
            (empty ? "|p" : "|v") + (hasComposition ? "|ime" : "") + placeholderKey;
        const float renderedTextHeight = multiline_ ? layout.contentHeight : textHeight;
        const float caretX = layout.clampedCursorX();
        const auto compositionRange = InputModel::selectionRange(state);
        const int compositionSize = hasComposition ? static_cast<int>(state.compositionText.size()) : 0;
        const auto documentIndex = [hasComposition, compositionRange, compositionSize](int index) {
            if (!hasComposition || index <= compositionRange.first) return index;
            if (index < compositionRange.first + compositionSize) return compositionRange.first;
            return index - compositionSize + compositionRange.second - compositionRange.first;
        };

        auto root = ui_.stack(id_)
            .size(width_, height_)
            .clip()
            // decorationRevision 必须进根元素的 dirtyKey：光标移动/主题切换只改装饰、
            // 不改 textRevision，漏了它整棵子树的 diff 就认为"没变"，装饰画旧值。
            .dirtyKey(InputModel::makeDirtyKey(state, focused, layout) +
                      "|d" + std::to_string(display.decorationRevision) +
                      (wordWrap_ ? "|wrap" : "|nowrap") +
                      (scrollbar_ ? "|bar" : "|no-bar") + placeholderKey);
        if (hasX_) {
            root.x(x_);
        }
        if (hasY_) {
            root.y(y_);
        }
        if (caretBlink_ && focused) {
            // 用定时器而不是逐帧回调：闪烁只要每半周期醒一次。
            // 框架会把这个定时器换算成"睡到那一刻"，不会让应用满速空转。
            root.onTimer(InputModel::kCaretBlinkHalfSeconds, [&state] {
                InputModel::caretBlinkTick(state);
            });
        }
        root.content([&] {
                auto hit = ui_.rect(hitId)
                    .size(width_, height_)
                    .color(style_.background)
                    .radius(style_.radius)
                    .border(1.0f, focused ? style_.focusBorder : style_.border)
                    .shadow(focused ? style_.shadow : core::Shadow{})
                    .transition(transition_)
                    .focusable()
                    .imeRect(caretX, caretY, 1.5f, std::max(1.0f, layout.cursorLineHeight()))
                    .onPress([&state, controlWidth, inset, baseInset, callbackLayout, documentIndex, onPointerHit](const core::PointerEvent& event, const core::Rect& bounds) {
                        const auto& layout = *callbackLayout;
                        state.lastBounds = bounds;
                        const InputModel::PointerHit hit =
                            layout.pointerHit(event.x, event.y, bounds, controlWidth, inset);
                        state.pressedGlyphLine = -1;
                        state.pressedLinkByte = -1;
                        if (hit.onGlyph && onPointerHit) {
                            state.selecting = false;
                            state.pressedGlyphLine = hit.lineNumber;
                            return;
                        }
                        if (hit.onLink && onPointerHit) {
                            // Do not navigate on mouse down or alter an existing selection.
                            state.selecting = false;
                            state.pressedLinkByte = documentIndex(hit.byteIndex);
                            state.pressedLinkLine = hit.lineNumber;
                            state.pressedLinkX = static_cast<float>(event.x);
                            state.pressedLinkY = static_cast<float>(event.y);
                            return;
                        }
                        // 点在块级图片上（S3f 批次 E）＝"看大图"，不是编辑：
                        // 光标原地不动、选区不动、不进拖选 —— 图片行若被移了光标，
                        // 该块立刻变成活动块、整行回到源码，图当场消失（批次 B 的既有行为）。
                        // 回调照发（onImage 置位），应用层决定开预览。
                        if (hit.onImage) {
                            if (onPointerHit) {
                                InputModel::PointerHit payload = hit;
                                payload.byteIndex = documentIndex(hit.byteIndex);
                                onPointerHit(payload);
                            }
                            return;
                        }
                        // 先记按下前的光标（显隐冻结值），再把光标移到点击处。
                        state.cursorBeforePress = state.cursor;
                        state.cursor = InputModel::clampUtf8Boundary(state.text, documentIndex(hit.byteIndex));
                        state.hasPreferredCursorX = false;
                        InputModel::clearSelection(state);
                        state.dragAnchor = state.cursor;
                        state.selecting = !hit.onGutter && !hit.onLink;
                        if (onPointerHit) {
                            // 回调拿到的是文档坐标的字节位置（IME 合成区间已映射回去），
                            // 与刚写进 state.cursor 的值同源 —— 应用层不必自己再换算。
                            InputModel::PointerHit payload = hit;
                            payload.byteIndex = documentIndex(hit.byteIndex);
                            onPointerHit(payload);
                        }
                    })
                    .onRelease([&state, callbackLayout, controlWidth, inset, documentIndex, onPointerHit](const core::PointerEvent& event, const core::Rect& bounds) {
                        const auto& layout = *callbackLayout;
                        const int pressed = state.pressedGlyphLine;
                        const int pressedLink = state.pressedLinkByte;
                        state.pressedLinkByte = -1;
                        state.pressedGlyphLine = -1;
                        // 松手 = 拖选结束：显隐从下一帧起重新跟随实时光标。
                        state.selecting = false;
                        if (pressedLink >= 0 && onPointerHit &&
                            bounds.contains(static_cast<float>(event.x), static_cast<float>(event.y)) &&
                            std::fabs(static_cast<float>(event.x) - state.pressedLinkX) < 4.0f &&
                            std::fabs(static_cast<float>(event.y) - state.pressedLinkY) < 4.0f) {
                            InputModel::PointerHit hit;
                            hit.onLink = true;
                            hit.byteIndex = pressedLink;
                            hit.lineNumber = state.pressedLinkLine;
                            onPointerHit(hit);
                        }
                        if (pressed >= 0 && bounds.contains(static_cast<float>(event.x), static_cast<float>(event.y))) {
                            auto hit = layout.pointerHit(event.x, event.y, bounds, controlWidth, inset);
                            if (hit.onGlyph && hit.lineNumber == pressed && onPointerHit) {
                                hit.byteIndex = documentIndex(hit.byteIndex);
                                onPointerHit(hit);
                            }
                        }
                    })
                    .onContextMenu([&state, controlWidth, inset, callbackLayout, documentIndex,
                                    onContextMenu](const core::PointerEvent& event, const core::Rect& bounds) {
                        const auto& layout = *callbackLayout;
                        // 与左键同一套落点换算（IME 区间映射回文档坐标）。
                        const InputModel::PointerHit hit =
                            layout.pointerHit(event.x, event.y, bounds, controlWidth, inset);
                        state.lastBounds = bounds;
                        state.contextLinkByte = hit.onLink ? documentIndex(hit.byteIndex) : -1;
                        if (!hit.onImage) {
                            const int byte = InputModel::clampUtf8Boundary(state.text, documentIndex(hit.byteIndex));
                            const int selBeg = std::min(state.selectionStart, state.selectionEnd);
                            const int selEnd = std::max(state.selectionStart, state.selectionEnd);
                            // 点在选区内：保留选区（右键菜单的格式命令要作用于它）；
                            // 选区外/无选区：光标移到落点（Windows/Obsidian 的惯例）。
                            if (byte < selBeg || byte >= selEnd) {
                                state.cursor = byte;
                                InputModel::clearSelection(state);
                                state.dragAnchor = state.cursor;
                            }
                        }
                        state.cursorBeforePress = state.cursor;
                        if (onContextMenu) {
                            onContextMenu(static_cast<float>(event.x), static_cast<float>(event.y));
                        }
                    })
                    .onFocusChanged([&state, onFocus](bool focused) {
                        if (!focused) {
                            state.selecting = false;
                            state.pressedGlyphLine = -1;
                            state.pressedLinkByte = -1;
                            if (!state.compositionText.empty()) {
                                state.compositionText.clear();
                                ++state.compositionRevision;
                            }
                        }
                        if (onFocus) onFocus(focused);
                    })
                    .onHover([&state](bool hovered) {
                        // 离开控件就清掉悬停行：行号位的折叠箭头只在该行悬停时显示。
                        if (!hovered) {
                            state.pointerHoverValid = false;
                            state.pointerHoverLine = -1;
                            state.pointerHoverLinkBeg = state.pointerHoverLinkEnd = -1;
                        }
                    })
                    .onMove([&state, callbackLayout, controlWidth, inset](const core::PointerEvent& event, const core::Rect& bounds) {
                        const auto& layout = *callbackLayout;
                        // 记下悬停位置并换算成可视行；行号位据此决定画箭头还是行号。
                        const float localY = static_cast<float>(event.y) - bounds.y;
                        const int line = layout.lineIndexFromY(localY);
                        const auto hit = layout.pointerHit(event.x, event.y, bounds, controlWidth, inset);
                        const int linkBeg = hit.onLink ? hit.linkBeg : -1;
                        const int linkEnd = hit.onLink ? hit.linkEnd : -1;
                        if (!state.pointerHoverValid || state.pointerHoverLine != line ||
                            state.pointerHoverLinkBeg != linkBeg || state.pointerHoverLinkEnd != linkEnd) {
                            state.pointerHoverY = localY;
                            state.pointerHoverLine = line;
                            state.pointerHoverValid = true;
                            state.pointerHoverLinkBeg = linkBeg;
                            state.pointerHoverLinkEnd = linkEnd;
                            return true;  // 悬停行变了：请求一帧重排
                        }
                        return false;
                    })
                    .onDragUpdate([&state, controlWidth, inset, textWidth, fontSize, fontFamily, allowMultiline, textHeight, callbackLayout, documentIndex](const core::dsl::DragEvent& event) {
                        const auto& layout = *callbackLayout;
                        const int oldPressedLinkByte = state.pressedLinkByte;
                        if (state.pressedLinkByte >= 0 &&
                            (std::fabs(static_cast<float>(event.x) - state.pressedLinkX) >= 4.0f ||
                             std::fabs(static_cast<float>(event.y) - state.pressedLinkY) >= 4.0f)) {
                            state.pressedLinkByte = -1;
                        }
                        if (!state.selecting) return oldPressedLinkByte != state.pressedLinkByte;
                        const int oldCursor = state.cursor;
                        const int oldSelectionStart = state.selectionStart;
                        const int oldSelectionEnd = state.selectionEnd;
                        const float oldHorizontalScroll = state.horizontalScroll;
                        const float oldVerticalScroll = state.verticalScroll;
                        state.cursor = InputModel::clampUtf8Boundary(state.text, documentIndex(layout.cursorFromPointer(event.x, event.y, state.lastBounds, controlWidth, inset)));
                        state.hasPreferredCursorX = false;
                        state.selectionStart = state.dragAnchor;
                        state.selectionEnd = state.cursor;
                        if (allowMultiline) {
                            InputModel::syncVerticalScroll(state, layout, textHeight);
                        } else {
                            InputModel::syncScroll(state, textWidth, fontFamily, fontSize);
                        }
                        // Keep stationary ticks for scroll-following, but avoid a
                        // full compose/paint when the selected byte and scrolls
                        // stayed unchanged (including the host's update(0)).
                        return oldCursor != state.cursor ||
                               oldSelectionStart != state.selectionStart ||
                               oldSelectionEnd != state.selectionEnd ||
                               oldHorizontalScroll != state.horizontalScroll ||
                               oldVerticalScroll != state.verticalScroll ||
                               oldPressedLinkByte != state.pressedLinkByte;
                    })
                    // 必须放在所有回调之后：onPress/onDrag/onMove 会把元素 cursor 隐式
                    // 设成 Hand（应用配置再把 Hand 映射成箭头），覆盖回去 = 悬停编辑区
                    // 显示 I-beam（否则全应用只有默认箭头，实测 2026-09-25）。
                    .cursor(cursor_)
                    .cursorAt([callbackLayout, controlWidth, inset, cursor = cursor_](const core::PointerEvent& event, const core::Rect& bounds) {
                        const auto& layout = *callbackLayout;
                        const auto hit = layout.pointerHit(event.x, event.y, bounds, controlWidth, inset);
                        if (hit.onGlyph || hit.onImage || hit.onLink) return core::CursorShape::Hand;
                        if (hit.onGutter) return core::CursorShape::Arrow;
                        return cursor;
                    });
                if (allowMultiline && (layout.maxVerticalScroll > 0.0f || !state.wordWrap)) {
                    hit.onScroll([&state, callbackLayout, fontSize](const core::ScrollEvent& event) {
                        const auto& layout = *callbackLayout;
                        const float step = std::max(12.0f, fontSize * 2.2f);
                        state.followCaret = false;
                        if(!state.wordWrap && event.x != 0) state.horizontalScroll=std::clamp(state.horizontalScroll-static_cast<float>(event.x)*step,0.0f,std::max(0.0f,layout.textWidth-layout.viewportWidth+fontSize));
                        state.verticalScroll = std::clamp(
                            state.verticalScroll - static_cast<float>(event.y) * step,
                            0.0f,
                            layout.maxVerticalScroll);
                    });
                }
                hit.onKeyEvent([&state, allowMultiline, onChange, onEnter, onEscape, width, inset, baseInset, textWidth, rightInset, fontSize, fontFamily, textHeight](const core::KeyEvent& event) {
                        if (!event.isDown()) {
                            return false;
                        }

                        // The IME owns candidate navigation, confirmation, cancellation,
                        // and shortcuts while preedit is active. Consume these here so
                        // they cannot move the document cursor or reach app commands.
                        if (!state.compositionText.empty()) return true;

                        state.followCaret = true;
                        // 兜底：键盘一动就结束"拖选冻结"（正常松手路径走 onRelease；
                        // 只有错过松手事件——比如窗口中途失焦——才靠这里自愈）。
                        state.selecting = false;
                        bool changed = false;
                        bool handled = true;
                        const bool shortcut = event.modifiers.shortcut();
                        const bool undo = shortcut && !event.modifiers.shift && event.key == core::InputKey::Z;
                        const bool redo = shortcut &&
                            (event.key == core::InputKey::Y ||
                             (event.modifiers.shift && event.key == core::InputKey::Z));
                        if (undo || redo) {
                            if (!state.compositionText.empty()) {
                                state.compositionText.clear();
                                ++state.compositionRevision;
                            }
                            changed = undo ? InputModel::undoEdit(state) : InputModel::redoEdit(state);
                            if (allowMultiline) {
                                state.horizontalScroll = 0.0f;
                                // 这里只为立刻把光标滚进视野。撤销可能让上一份装饰失效，
                                // 所以按"未装饰"的排版估一次位置；下一帧 build() 会用新装饰重新夹取。
                                const InputLayout nextLayout = InputLayout::build(
                                    state,
                                    textWidth,
                                    textHeight,
                                    width,
                                    inset,
                                    rightInset,
                                    0.0f,
                                    fontSize * 1.2f,
                                    fontFamily,
                                    fontSize,
                                    allowMultiline);
                                InputModel::syncVerticalScroll(state, nextLayout, textHeight);
                            } else {
                                InputModel::syncScroll(state, textWidth, fontFamily, fontSize);
                            }
                            if (changed && onChange) {
                                onChange(state.text);
                            }
                            return true;
                        }

                        if (shortcut && event.key == core::InputKey::A) {
                            state.selectionStart = 0;
                            state.selectionEnd = static_cast<int>(state.text.size());
                            state.cursor = state.selectionEnd;
                        } else if (shortcut && event.key == core::InputKey::C) {
                            InputModel::copySelection(state);
                        } else if (shortcut && event.key == core::InputKey::X) {
                            if (InputModel::hasTextSelection(state)) {
                                InputModel::copySelection(state);
                                // 删除本身在 eraseSelection 里记账：调用方不再预告一次，
                                // 否则"快照早于删除/晚于删除"会让撤销落到错误的文本上。
                                InputModel::eraseSelection(state);
                                changed = true;
                            }
                        } else if (shortcut && event.key == core::InputKey::V) {
                            // Clipboard text is delivered separately through onTextInput.
                        } else if (event.key == core::InputKey::Left) {
                            InputModel::moveCursor(state, -1, event.modifiers.shift, fontFamily, fontSize, allowMultiline, textWidth);
                        } else if (event.key == core::InputKey::Right) {
                            InputModel::moveCursor(state, 1, event.modifiers.shift, fontFamily, fontSize, allowMultiline, textWidth);
                        } else if (event.key == core::InputKey::Up) {
                            if (allowMultiline) {
                                InputModel::moveCursorVertical(state, -1, event.modifiers.shift, fontFamily, fontSize, textWidth, textHeight);
                            } else {
                                // 单行输入（查找/替换、重命名等）没有"上一行"：↑ 复用
                                // Home 的实现跳到行首；多行的边界折行跳转在
                                // moveCursorVertical 内统一处理，两种场景行为对齐。
                                InputModel::moveCursorTo(state, 0, event.modifiers.shift);
                            }
                        } else if (event.key == core::InputKey::Down) {
                            if (allowMultiline) {
                                InputModel::moveCursorVertical(state, 1, event.modifiers.shift, fontFamily, fontSize, textWidth, textHeight);
                            } else {
                                // 单行 ↓ 同理：复用 End 的实现跳到行尾。
                                InputModel::moveCursorTo(state, static_cast<int>(state.text.size()), event.modifiers.shift);
                            }
                        } else if ((event.key == core::InputKey::PageUp || event.key == core::InputKey::PageDown) && allowMultiline) {
                            InputModel::moveCursorPage(state, event.key == core::InputKey::PageUp ? -1 : 1,
                                event.modifiers.shift, fontFamily, fontSize, textWidth, textHeight);
                        } else if (event.key == core::InputKey::Home) {
                            if (allowMultiline && !shortcut) {
                                InputModel::moveCursorToLineEdge(state, false, event.modifiers.shift, fontFamily, fontSize, textWidth);
                            } else {
                                InputModel::moveCursorTo(state, 0, event.modifiers.shift);
                            }
                        } else if (event.key == core::InputKey::End) {
                            if (allowMultiline && !shortcut) {
                                InputModel::moveCursorToLineEdge(state, true, event.modifiers.shift, fontFamily, fontSize, textWidth);
                            } else {
                                InputModel::moveCursorTo(state, static_cast<int>(state.text.size()), event.modifiers.shift);
                            }
                        } else if (event.key == core::InputKey::Delete) {
                            if (InputModel::hasTextSelection(state)) {
                                InputModel::eraseSelection(state);
                                changed = true;
                            } else if (state.cursor < static_cast<int>(state.text.size())) {
                                const int next = InputModel::nextCursorIndex(state, fontFamily, fontSize, allowMultiline, textWidth);
                                InputModel::eraseRange(state, state.cursor, next);
                                changed = true;
                            }
                        } else if (event.key == core::InputKey::Backspace) {
                            if (InputModel::hasTextSelection(state)) {
                                InputModel::eraseSelection(state);
                                changed = true;
                            } else if (state.cursor > 0) {
                                const int previous = InputModel::prevCursorIndex(state, fontFamily, fontSize, allowMultiline, textWidth);
                                InputModel::eraseRange(state, previous, state.cursor);
                                changed = true;
                            }
                        } else if (event.key == core::InputKey::Enter) {
                            if (allowMultiline) {
                                InputModel::insertAtCursor(state, "\n");
                                changed = true;
                            } else if (onEnter) {
                                onEnter();
                            }
                        } else if (event.key == core::InputKey::Escape) {
                            if (onEscape) {
                                onEscape();
                            } else if (onEnter) {
                                onEnter();
                            } else {
                                // 没设 onEnter 的多行输入不消费 Esc：全屏预览这类应用级
                                // 弹层要能在编辑区聚焦时用 Esc 关闭（S3f 批次 E）。
                                // 已设 onEnter 的使用者（地址栏等）行为不变。
                                handled = false;
                            }
                        } else {
                            handled = false;
                        }
                        if (allowMultiline) {
                            state.horizontalScroll = 0.0f;
                        } else {
                            InputModel::syncScroll(state, textWidth, fontFamily, fontSize);
                        }
                        if (changed && onChange) {
                            onChange(state.text);
                        }
                        return handled;
                    })
                    .onTextInput([&state, allowMultiline, onChange, onImagePaste, width, inset, baseInset, textWidth, fontSize, fontFamily](const core::TextInputEvent& event) {
                        state.followCaret = true;
                        state.selecting = false;
                        bool changed = false;
                        const std::string nextComposition = event.composing
                            ? InputModel::filteredText(event.compositionText, allowMultiline)
                            : std::string{};
                        const auto insertText = [&](const std::string& text) {
                            if (text.empty()) {
                                return;
                            }
                            if (!state.compositionText.empty()) {
                                state.compositionText.clear();
                                ++state.compositionRevision;
                            }
                            // 记账下沉到 insertAtCursor：这里再 push 一次就是双 push
                            // （选区擦除 + 插入会被拆成两条记录，撤销只剩半截）。
                            InputModel::insertAtCursor(state, InputModel::filteredText(text, allowMultiline));
                            changed = true;
                        };
                        insertText(event.text);
                        insertText(event.pasteText);
                        // A result and a new preedit can arrive together. Publish the
                        // new preedit after the result advances the document cursor.
                        if (state.compositionText != nextComposition) {
                            state.compositionText = nextComposition;
                            ++state.compositionRevision;
                        }
                        // Ctrl+V 到了但剪贴板没有文本：交给宿主决定是否走图片粘贴
                        // （编辑器实例挂回调；查找栏/弹窗输入不挂，行为不变）。
                        if (event.pasteRequested && event.pasteText.empty() && onImagePaste) {
                            onImagePaste();
                        }

                        if (allowMultiline) {
                            state.horizontalScroll = 0.0f;
                        } else {
                            InputModel::syncScroll(state, textWidth, fontFamily, fontSize);
                        }
                        if (changed && onChange) {
                            onChange(state.text);
                        }
                    })
                    .build();

                // ── 块级矩形（代码块底色 / 引用竖条）──
                // 必须画在 textViewport **之外**：视口是内缩过的、而且 clip 的
                // （竖条要贴到左侧留白里，画在视口内既没有那块地方、也会被裁掉）。
                // 所以这里用的是**控件坐标**（再加 inset / textY）。
                if (table != nullptr && !table->empty() && !layout.lineList().empty()) {
                    const auto& blockLines = layout.lineList();
                    const auto& blockGeometry = layout.geometryTable();
                    const int blockLineCount = static_cast<int>(blockLines.size());
                    const bool haveBlockGeometry = blockGeometry.count() > 0;
                    const auto lineTopAt = [&](int index) {
                        return haveBlockGeometry ? blockGeometry.top(index)
                                                 : static_cast<float>(index) * textLineHeight;
                    };
                    const auto lineBottomAt = [&](int index) {
                        return haveBlockGeometry ? blockGeometry.bottom(index)
                                                 : static_cast<float>(index + 1) * textLineHeight;
                    };
                    const int blockFirstVisible = haveBlockGeometry
                        ? std::clamp(blockGeometry.firstVisibleLine(state.verticalScroll), 0, blockLineCount - 1)
                        : 0;
                    const int blockLastVisible = haveBlockGeometry
                        ? std::clamp(blockGeometry.lastVisibleLine(state.verticalScroll, textHeight),
                                     blockFirstVisible, blockLineCount - 1)
                        : blockLineCount - 1;

                    // 底色整块画一个矩形：逐行画的话每行都要圆角，块内部会露出一排圆角缺口。
                    // 块可能横跨视口，所以向前/后各多扫一段来定位真正的块边界。
                    // 折叠（S3f 批次 C）的 hidden 行不算块的一部分：整块被折叠时
                    // 各行 top 已挤成一点，这里再跳过就不会画出 0 高的底色矩形。
                    constexpr int kBlockScanMargin = 64;
                    const int scanBeg = std::max(0, blockFirstVisible - kBlockScanMargin);
                    const int scanEnd = std::min(blockLineCount - 1, blockLastVisible + kBlockScanMargin);
                    int blockStart = -1;
                    for (int index = scanBeg; index <= scanEnd + 1; ++index) {
                        const bool inside = index <= scanEnd && !blockLines[index].hidden &&
                            blockLines[index].box.background.a > 0.0f;
                        if (inside && blockStart < 0) {
                            blockStart = index;
                        }
                        if (blockStart >= 0 && (!inside || blockLines[index].box.backgroundBlockLast)) {
                            const int blockLast = inside ? index : index - 1;
                            const float blockTop = textY + lineTopAt(blockStart) - state.verticalScroll;
                            const float blockBottom = textY + lineBottomAt(blockLast) - state.verticalScroll;
                            if (blockBottom > 0.0f && blockTop < height_) {
                                const float radius = blockLines[blockStart].box.backgroundRadius > 0.0f
                                    ? blockLines[blockStart].box.backgroundRadius
                                    : std::max(2.0f, textLineHeight * 0.18f);
                                // 表格行的底色带收进表格自身宽度（与网格线对齐）；
                                // 代码块/引用等仍横跨整个内容列。
                                float bandX = inset;
                                float bandWidth = textWidth;
                                if (layout.tables != nullptr && blockLines[blockStart].tableId >= 0) {
                                    const input_detail::InputModel::TableColumns* table =
                                        layout.tableColumnsFor(blockLines[blockStart].tableId);
                                    if (table != nullptr && table->count() > 0) {
                                        bandX = inset + std::max(0.0f, table->x[0] - table->padding);
                                        bandWidth = table->total + table->padding * 2.0f;
                                    }
                                }
                                ui_.rect(id_ + ".linebg." + std::to_string(blockStart))
                                    .position(bandX, blockTop)
                                    // 底色从文本原点起（不横跨行号列）：行号区保持编辑器底色，
                                    // 与代码块底色区分开。横跨的设计已按 2026-09-25 反馈废除。
                                    .size(bandWidth, blockBottom - blockTop)
                                    .color(blockLines[blockStart].box.background)
                                    .radius(radius)
                                    .build();
                            }
                            blockStart = -1;
                        }
                    }

                    // 竖条逐行画：等宽竖直矩形，相邻行紧贴就自然连成一条 —— 不需要块边界，也没有圆角。
                    // C1：barCount >= 2 画多条（嵌套引用），从单条的位置往左排开 ——
                    // 文字位置不随条数变（竖条本来就不占文字位置），gap=0 约束不碰。
                    for (int index = blockFirstVisible; index <= blockLastVisible; ++index) {
                        const input_detail::LineBoxStyle& box = blockLines[index].box;
                        if (box.barColor.a <= 0.0f || box.barWidth <= 0.0f || blockLines[index].hidden) {
                            continue;
                        }
                        const float barTop = textY + lineTopAt(index) - state.verticalScroll;
                        const int barCount = box.barCount > 0 ? static_cast<int>(box.barCount) : 1;
                        const float barStride = (blockLines[index].fontSize > 0.0f
                            ? blockLines[index].fontSize : fontSize) * 1.2f;
                        for (int bar = 0; bar < barCount; ++bar) {
                            ui_.rect(id_ + ".linebar." + std::to_string(index) +
                                     (bar > 0 ? "." + std::to_string(bar) : ""))
                                .position((box.barCount > 0 ? inset : std::max(0.0f, inset - box.barWidth - 6.0f)) +
                                              static_cast<float>(bar) * barStride,
                                          barTop)
                                .size(box.barWidth, lineBottomAt(index) - lineTopAt(index))
                                .color(box.barColor)
                                .build();
                        }
                    }

                    // ── 表格网格线（2026-09-25）──
                    // 列几何在 layout.tables（一张表一份，行间共享），行身份在
                    // line.tableId（装饰层给）。竖线逐行画、横线只在行界画 ——
                    // 相邻行 y 紧贴，1px 线自然连成整片。颜色 = 装饰层的 gridColor
                    // （style.divider）；表头下的分隔行自带 3px 底色横条当边框，
                    // 横线在那里让位（竖线照穿，列的结构不断）。
                    if (layout.tables != nullptr) {
                        const int gridScanBeg = std::max(0, blockFirstVisible - kBlockScanMargin);
                        const int gridScanEnd = std::min(blockLineCount - 1, blockLastVisible + kBlockScanMargin);
                        constexpr float kGridLineWidth = 1.0f;
                        for (int index = gridScanBeg; index <= gridScanEnd; ++index) {
                            const input_detail::InputModel::TextLine& row = blockLines[static_cast<std::size_t>(index)];
                            if (row.tableId < 0 || row.hidden || row.box.gridColor.a <= 0.0f) {
                                continue;
                            }
                            const input_detail::InputModel::TableColumns* table =
                                layout.tableColumnsFor(row.tableId);
                            if (table == nullptr || table->count() == 0) {
                                continue;
                            }
                            const float rowTop = textY + lineTopAt(index) - state.verticalScroll;
                            const float rowBottom = textY + lineBottomAt(index) - state.verticalScroll;
                            if (rowBottom <= 0.0f || rowTop >= height_) {
                                continue;
                            }
                            // 竖线：表格外缘两根 + 列与列之间（列文字区左侧一个 padding 处）。
                            for (int c = 0; c <= table->count(); ++c) {
                                const float boundary = c == 0
                                    ? table->x[0] - table->padding
                                    : (c == table->count()
                                           ? table->total + table->padding
                                           : table->x[static_cast<std::size_t>(c)] - table->padding);
                                ui_.rect(id_ + ".grid.v." + std::to_string(index) + "." + std::to_string(c))
                                    .position(inset + boundary, rowTop)
                                    .size(kGridLineWidth, rowBottom - rowTop)
                                    .color(row.box.gridColor)
                                    .build();
                            }
                            const auto drawGridHLine = [&](float y, const char* suffix) {
                                ui_.rect(id_ + ".grid.h." + std::to_string(index) + suffix)
                                    .position(inset + table->x[0] - table->padding, y)
                                    .size(table->total + table->padding * 2.0f, kGridLineWidth)
                                    .color(row.box.gridColor)
                                    .build();
                            };
                            const bool prevSameTable = index > 0 &&
                                blockLines[static_cast<std::size_t>(index - 1)].tableId == row.tableId &&
                                !blockLines[static_cast<std::size_t>(index - 1)].hidden;
                            const bool nextSameTable = index + 1 < blockLineCount &&
                                blockLines[static_cast<std::size_t>(index + 1)].tableId == row.tableId &&
                                !blockLines[static_cast<std::size_t>(index + 1)].hidden;
                            // 表顶横线（本行上面不是同一张表）。
                            if (!prevSameTable) {
                                drawGridHLine(rowTop, "t");
                            }
                            // 行间横线：两侧都是普通表格行才画；贴着分隔行的那两条
                            // 由 3px 底色横条承担，表底（!nextSameTable）要收口。
                            if (!nextSameTable) {
                                drawGridHLine(rowBottom - kGridLineWidth, "b");
                            } else if (!row.tableSeparator &&
                                       !blockLines[static_cast<std::size_t>(index + 1)].tableSeparator &&
                                       blockLines[static_cast<std::size_t>(index + 1)].lineStart) {
                                drawGridHLine(rowBottom - kGridLineWidth, "b");
                            }
                        }
                    }
                }

                // ── 行号列 ──
                // 画在 textViewport 之外（控件坐标）：右对齐到 [inset - gutterWidth, inset - 8]，
                // 即贴着**正文起点**（inset 已含可读行宽的居中量，2026-09-26 起跟随内容列
                // 移动，对齐 Obsidian；不限制行宽时 inset = baseInset + gutterWidth，槽位
                // 退回左缘原位）。只在"可视行是源行第一条"时画（软换行的续行不画）。
                // y 跟随行的几何表。折叠箭头只在**悬停行**上是箭头（对齐 Obsidian），
                // 其余时候是行号。两者都与该行文字的墨迹中心垂直对齐：行盒高度可以大于
                // 文字（标题行的 padding-top），顶对齐会让行号浮到盒子顶部。
                if (lineNumbers_ && multiline_ && !empty && !layout.lineList().empty()) {
                    const auto& numberLines = layout.lineList();
                    const auto& numberGeometry = layout.geometryTable();
                    const bool numberHasGeometry = numberGeometry.count() > 0;
                    const int numberLineCount = static_cast<int>(numberLines.size());
                    const int numberFirst = numberHasGeometry
                        ? std::clamp(numberGeometry.firstVisibleLine(state.verticalScroll), 0, numberLineCount - 1)
                        : 0;
                    const int numberLast = numberHasGeometry
                        ? std::clamp(numberGeometry.lastVisibleLine(state.verticalScroll, textHeight),
                                     numberFirst, numberLineCount - 1)
                        : numberLineCount - 1;
                    for (int index = numberFirst; index <= numberLast; ++index) {
                        const auto& line = numberLines[static_cast<std::size_t>(index)];
                        if (!line.lineStart || line.hidden) {
                            continue;
                        }
                        const float linePixelHeight = numberHasGeometry ? numberGeometry.height(index) : textLineHeight;
                        const float y = (numberHasGeometry ? numberGeometry.top(index)
                                                           : static_cast<float>(index) * textLineHeight) -
                            state.verticalScroll;
                        if (y + linePixelHeight < 0.0f || y > textHeight) {
                            continue;
                        }
                        const float alignFontSize = line.fontSize > 0.0f ? line.fontSize : fontSize;
                        // 行号（与折叠箭头）不画在"装不下这一行字"的行上（T19 视觉修正 1）：
                        // 典型是表格分隔行 —— 装饰层把行高压到 3px、整行文字藏进洞里，行号
                        // 槽（≈1.2em）画上去只会糊在分隔条上。判据在 model（gutterLineUsable），
                        // 单测直接打它。已知边界：极大字号 + 极小图片行同样会被跳过。
                        if (!input_detail::InputModel::gutterLineUsable(
                                line, linePixelHeight, alignFontSize)) {
                            continue;
                        }
                        // 文字墨迹中心的估算：基线 ≈ 0.78em（归一后 ascent），墨迹中心再上提
                        // 约半个 cap 高 —— 合计 ≈ 0.55em，随行字号走。
                        // 锚点（带顶）与选区背景走同一个纯几何 helper：行号槽、文字、
                        // 选区三者因此不会各猜一个 textShiftY 偏移。
                        const auto numberBand = input_detail::lineTextBand(y, linePixelHeight, line.textShiftY);
                        const float textCenterY = textY + numberBand.top + numberBand.height * 0.5f;
                        const float slotHeight = alignFontSize * 1.2f;
                        const float slotY = textCenterY - slotHeight * 0.5f;
                        const bool hoverOnThisLine = state.pointerHoverValid && index == state.pointerHoverLine;
                        const float gutterX = inset - gutterWidth;
                        if (line.gutterGlyph.codepoint != 0 && hoverOnThisLine) {
                            const auto glyphColor = line.gutterGlyph.color.a > 0.0f
                                ? line.gutterGlyph.color : lineNumberColor_;
                            const float iconSize = std::max(6.0f, slotHeight * 0.42f);
                            const float iconX = gutterX + std::max(0.0f, gutterWidth - 8.0f - iconSize);
                            const float iconY = slotY + (slotHeight - iconSize) * 0.5f;
                            if (line.gutterGlyph.codepoint == 0xF054) {
                                vector_icon::drawChevronRight(
                                    ui_, id_ + ".gutterglyph." + std::to_string(index),
                                    iconX, iconY, iconSize, glyphColor);
                                continue;
                            }
                            if (line.gutterGlyph.codepoint == 0xF078) {
                                vector_icon::drawChevronDown(
                                    ui_, id_ + ".gutterglyph." + std::to_string(index),
                                    iconX, iconY, iconSize, glyphColor);
                                continue;
                            }
                            // Preserve custom LineGlyph codepoints for generic component users.
                            ui_.text(id_ + ".gutterglyph." + std::to_string(index))
                                .position(gutterX, slotY)
                                .size(std::max(1.0f, gutterWidth - 8.0f), slotHeight)
                                .dirtyKey(textDirtyKey + "|G" + std::to_string(index))
                                .icon(static_cast<unsigned int>(line.gutterGlyph.codepoint))
                                .fontSize(std::max(9.0f, alignFontSize * 0.62f))
                                .color(glyphColor)
                                .horizontalAlign(core::HorizontalAlign::Right)
                                .verticalAlign(core::VerticalAlign::Center)
                                .build();
                            continue;
                        }
                        ui_.text(id_ + ".lineno." + std::to_string(index))
                            .position(gutterX, slotY)
                            .size(std::max(1.0f, gutterWidth - 8.0f), slotHeight)
                            .dirtyKey(textDirtyKey + "|n" + std::to_string(index))
                            .text(std::to_string(line.lineNumber))
                            .fontSize(fontSize)
                            .fontFamily(fontFamily_)
                            .color(lineNumberColor_.a > 0.0f ? lineNumberColor_ : style_.placeholder)
                            .wrap(false)
                            .horizontalAlign(core::HorizontalAlign::Right)
                            .verticalAlign(core::VerticalAlign::Center)
                            .build();
                    }
                }

                // 裁剪框比文本行盒**上探 inkOverhang**：否则首行的上升部会被裁（实测 2 物理 px）。
                // 子元素靠 padding 下移同一量，于是它们的坐标原点仍是 textY —— 内部所有
                // `- textY` / `- inset` 的相对定位不用改。单行模式 padding = 0，
                // position 是居中的行盒 textY，文字图元独立使用真实墨迹居中。
                ui_.stack(id_ + ".textViewport")
                    .position(inset, textViewportTop)
                    .size(textWidth, textHeight)
                    .padding(0.0f, textViewportPadTop, 0.0f, 0.0f)
                    .clip()
                    .content([&] {
                        if (hasSelection) {
                            for (size_t index = 0; index < layout.selectionRects.size(); ++index) {
                                const auto& selectionRect = layout.selectionRects[index];
                                ui_.rect(id_ + ".selection." + std::to_string(index))
                                    .position(selectionRect.x - inset, selectionRect.y - textY)
                                    .size(selectionRect.width, selectionRect.height)
                                    .color(theme::withAlpha(style_.cursor, hasComposition ? 0.12f : 0.24f))
                                    .radius(multiline_ ? 0.0f : 3.0f)
                                    .build();
                                if (hasComposition) {
                                    const float underlineLineHeight = selectionRect.lineHeight > 0.0f
                                        ? selectionRect.lineHeight : textLineHeight;
                                    ui_.rect(id_ + ".composition.underline." + std::to_string(index))
                                        .position(selectionRect.x - inset, selectionRect.y - textY + underlineLineHeight - 2.f)
                                        .size(selectionRect.width, 1.f).color(style_.cursor).build();
                                }
                            }
                        }

                        if (multiline_ && !empty && !layout.lineList().empty()) {
                            ui_.stack(id_ + ".horizontal.content").position(-layout.scroll,0).size(layout.visibleTextWidth,textHeight).content([&] {
                            const auto& lines = layout.lineList();
                            const auto& geometry = layout.geometryTable();
                            const bool hasGeometry = geometry.count() > 0;
                            const int lineCount = static_cast<int>(lines.size());
                            const int firstLine = hasGeometry
                                ? std::clamp(geometry.firstVisibleLine(state.verticalScroll), 0, lineCount - 1)
                                : 0;
                            const int endLine = hasGeometry
                                ? std::clamp(geometry.lastVisibleLine(state.verticalScroll, textHeight), firstLine, lineCount - 1)
                                : lineCount - 1;
                            for (int index = firstLine; index <= endLine; ++index) {
                                const auto& line = lines[static_cast<std::size_t>(index)];
                                // 折叠（S3f 批次 C）：hidden 行 0 高且不画 —— 光靠下面的
                                // 视口裁剪挡不住它（0 高行的 y 仍在视口内会画到折叠点上）。
                                if (line.hidden) {
                                    continue;
                                }
                                const float linePixelHeight = hasGeometry ? geometry.height(index) : textLineHeight;
                                const float y = (hasGeometry ? geometry.top(index)
                                                             : static_cast<float>(index) * textLineHeight) - state.verticalScroll;
                                if (y + linePixelHeight < 0.0f || y > textHeight) {
                                    continue;
                                }
                                // 有隐藏区间的行渲染"投影文本"（标记已被删掉）。"光标所在块显示源码"
                                // 由上层表达——对那几行不返回 holes，组件本身不做这个判断。
                                // 行级文字色（标题/代码块）与行内样式段都由上层给，组件只照着画。
                                const core::Color lineColor =
                                    line.color.a > 0.0f ? line.color : style_.text;
                                const float lineFontSize = line.fontSize > 0.0f ? line.fontSize : fontSize;
                                // Center the nominal em box inside the shared selection band.
                                // Styled runs share an origin and baseline; checkboxes and the
                                // caret center independently within that same band.
                                const input_detail::LineTextBand textBand =
                                    input_detail::lineTextBand(y, linePixelHeight, line.textShiftY);
                                const float textOrigin = input_detail::lineTextOrigin(textBand, lineFontSize);
                                if (line.box.horizontalRuleThickness > 0 && line.box.gridColor.a > 0.0f) {
                                    const float thickness =
                                        std::min(static_cast<float>(line.box.horizontalRuleThickness), linePixelHeight);
                                    ui_.rect(id_ + ".hr." + std::to_string(index))
                                        .position(0.0f, y + (linePixelHeight - thickness) * 0.5f)
                                        .size(textWidth, thickness)
                                        .color(line.box.gridColor)
                                        .build();
                                }
                                // 行首图元（任务复选框 / C1 列表 marker）。文字已经按
                                // glyph.advance 右移过（见 InputModel::applyLineGlyph），
                                // 所以这里只按 x = 0 画图元。
                                // Y 跟随 textShiftY（标题行的 padding-top 在文字上方）。
                                // 文本 marker（glyph.text 非空，"•" / 序号）只在
                                // lineStart 段画一次 —— 软换行续段与物理续行只共享
                                // advance 平移，不重复画 marker；在 advance 框内右对齐，
                                // 序号比源码前缀宽/窄时正文列不动。codepoint != 0 走
                                // 图标路（任务复选框，沿用既有行为在每段画）。
                                if (line.glyph.codepoint != 0 || !line.glyph.text.empty()) {
                                    if (!line.glyph.text.empty()) {
                                        if (line.lineStart && line.glyph.text == "•") {
                                            // A native dot has no font bearings or glyph clip box.
                                            const float dot = std::max(3.0f, lineFontSize * 0.28f);
                                            const float gap = lineFontSize * 0.30f;
                                            ui_.rect(id_ + ".listmarker." + std::to_string(index))
                                                .position(line.contentIndent + std::max(0.0f,
                                                    line.glyph.advance - gap - dot),
                                                    textBand.top + (textBand.height - dot) * 0.5f)
                                                .size(dot, dot).radius(dot * 0.5f)
                                                .color(line.glyph.color.a > 0.0f ? line.glyph.color : lineColor)
                                                .build();
                                        } else if (line.lineStart) {
                                            ui_.text(id_ + ".listmarker." + std::to_string(index))
                                                .position(line.contentIndent, textOrigin)
                                                .size(std::max(1.0f, line.glyph.advance -
                                                    (line.glyph.text == "•" ? 0.0f : lineFontSize * 0.28f)), linePixelHeight)
                                                .dirtyKey(textDirtyKey + "|lm" + std::to_string(index))
                                                .text(line.glyph.text)
                                                .fontSize(lineFontSize)
                                                .fontFamily(line.fontFamily.empty() ? fontFamily_ : line.fontFamily)
                                                .lineHeight(textBand.height)
                                                .color(line.glyph.color.a > 0.0f ? line.glyph.color : lineColor)
                                                .horizontalAlign(core::HorizontalAlign::Right)
                                                .verticalAlign(core::VerticalAlign::Top)
                                                .wrap(false)
                                                .build();
                                        }
                                    } else if (line.lineStart && line.glyph.checkbox) {
                                        const float side = lineFontSize;
                                        const float checkboxY = textBand.top + (textBand.height - side) * 0.5f;
                                        const auto checkboxColor = line.glyph.color.a > 0.0f ? line.glyph.color : lineColor;
                                        ui_.rect(id_ + ".checkbox." + std::to_string(index))
                                            .position(line.contentIndent, checkboxY)
                                            .size(side, side)
                                            .radius(std::max(2.0f, side * 0.2f))
                                            .border(line.glyph.checked ? 0.0f : 1.0f, checkboxColor)
                                            .color(line.glyph.checked ? checkboxColor : core::Color{0, 0, 0, 0})
                                            .build();
                                        if (line.glyph.checked) {
                                            vector_icon::drawCheckmark(
                                                ui_, id_ + ".checkbox.check." + std::to_string(index),
                                                line.contentIndent, checkboxY, side, style_.background);
                                        }
                                    } else if (line.lineStart) {
                                        ui_.text(id_ + ".glyph." + std::to_string(index))
                                            .position(line.contentIndent, textBand.top)
                                            .size(std::max(1.0f, line.glyph.advance), textBand.height)
                                            .dirtyKey(textDirtyKey + "|g" + std::to_string(index))
                                            .icon(static_cast<unsigned int>(line.glyph.codepoint))
                                            .fontSize(lineFontSize)
                                            .color(line.glyph.color.a > 0.0f ? line.glyph.color : lineColor)
                                            .horizontalAlign(core::HorizontalAlign::Left)
                                            .verticalAlign(core::VerticalAlign::Center)
                                            .build();
                                    }
                                }
                                // 行首图元 / 行内容左缩进把文字整体推开：run.x / caretX 已经
                                // 平移过（见 InputModel::applyLineGlyph / applyLineIndent），
                                // 但**没有样式段的行**是硬编码画在 x = 0 的，所以这里也要
                                // 补上同一个偏移。文本 marker 的 advance 已在 applyLineGlyph
                                // 解析进 line.glyph.advance（图元占位对文本路与图标路一视同仁）。
                                const float lineTextOffset =
                                    (!line.glyph.text.empty() || line.glyph.codepoint != 0
                                         ? line.glyph.advance
                                         : 0.0f) +
                                    line.contentIndent;
                                // 代码块起始围栏行的语言标签（Obsidian LP 在底色条右上角画
                                // 一个弱化的语言名）。右对齐、比代码小一档、颜色取代码文字色
                                // 的 75% —— Obsidian 用的是 --text-muted（暗色 #b3b3b3，参考图里
                                // 标签最亮像素 179），本组件没有这一档 token，用淡化近似。
                                if (!line.languageLabel.empty()) {
                                    const float labelInset = std::max(8.0f, lineFontSize);
                                    ui_.text(id_ + ".lang." + std::to_string(index))
                                        .position(labelInset, textBand.top)
                                        .size(std::max(1.0f, layout.visibleTextWidth - labelInset * 2.0f),
                                              linePixelHeight)
                                        .dirtyKey(textDirtyKey + "|lang" + std::to_string(index))
                                        .text(line.languageLabel)
                                        .fontSize(std::max(9.0f, lineFontSize * 0.85f))
                                        .fontFamily(line.fontFamily.empty() ? fontFamily_ : line.fontFamily)
                                        .lineHeight(linePixelHeight)
                                        .color(theme::withOpacity(lineColor, 0.75f))
                                        .horizontalAlign(core::HorizontalAlign::Right)
                                        .verticalAlign(core::VerticalAlign::Center)
                                        .wrap(false)
                                        .build();
                                }
                                if (line.runs.empty()) {
                                    ui_.text(id_ + ".text." + std::to_string(index))
                                        .position(lineTextOffset, textOrigin)
                                        .size(layout.visibleTextWidth, textBand.height)
                                        .dirtyKey(textDirtyKey + "|" + std::to_string(index))
                                        .text(input_detail::projectText(display.text, line.start, line.end, line.holes))
                                        .fontSize(lineFontSize)
                                        .fontFamily(line.fontFamily.empty() ? fontFamily_ : line.fontFamily)
                                        .lineHeight(textBand.height)
                                        .color(lineColor)
                                        .wrap(false)
                                        .verticalAlign(core::VerticalAlign::Top)
                                        .build();
                                } else {
                                    // 逐段画。段宽由测量阶段给出（各段字体参数不同，宽度只能实测），
                                    // 所以这里只负责"按 x 摆放 + 上色 + 点缀（底色块 / 删除线）"。
                                    // Horizontal clipping in the renderer does not avoid creating
                                    // nodes/layouts for every token of an unwrapped source line.
                                    // Keep complete runs for editing/hit tests, but emit only those
                                    // near the viewport; the margin retains ink/chip overhang.
                                    const float runMargin = std::max(32.0f, lineFontSize * 2.0f);
                                    const float runViewportLeft = layout.scroll - runMargin;
                                    const float runViewportRight = layout.scroll + textWidth + runMargin;
                                    for (std::size_t runIndex = 0; runIndex < line.runs.size(); ++runIndex) {
                                        const input_detail::TextRun& run = line.runs[runIndex];
                                        if (run.x + run.width < runViewportLeft || run.x > runViewportRight) {
                                            continue;
                                        }
                                        const std::string runId = id_ + ".text." + std::to_string(index) + "r" +
                                            std::to_string(runIndex);
                                        const std::string runText = input_detail::projectText(
                                            display.text, run.beg, run.end, line.holes);
                                        if (runText.empty()) {
                                            continue;
                                        }
                                        const core::Color runColor =
                                            run.style.color.a > 0.0f ? run.style.color : lineColor;
                                        if (run.style.link) {
                                            const bool hoveredLink = state.pointerHoverLinkBeg == run.beg &&
                                                state.pointerHoverLinkEnd == run.end;
                                            ui_.rect(runId + ".linkhover")
                                                .position(run.x - 1.0f, textBand.top)
                                                .size(run.width + 2.0f, textBand.height)
                                                .radius(3.0f)
                                                .color(theme::withAlpha(style_.cursor, hoveredLink ? 0.12f : 0.0f))
                                                .transition(core::Transition::make(0.10f, core::Ease::OutCubic))
                                                .animate(core::AnimProperty::Color)
                                                .build();
                                        }
                                        if (run.style.background.a > 0.0f) {
                                            // The chip follows the centered text origin, preserving
                                            // its padding and baseline beside the other runs.
                                            const float chipPad = std::max(1.0f, lineFontSize * 0.10f);
                                            ui_.rect(runId + ".bg")
                                                .position(run.x - chipPad, textOrigin + lineFontSize * 0.06f)
                                                .size(run.width + chipPad * 2.0f, lineFontSize * 0.80f)
                                                .color(run.style.background)
                                                .radius(std::max(2.0f, lineFontSize * 0.18f))
                                                .build();
                                        }
                                        ui_.text(runId)
                                            .position(run.x, textOrigin)
                                            .size(run.width + 16.0f, textBand.height)
                                            .dirtyKey(textDirtyKey + "|" + std::to_string(index) + "r" +
                                                      std::to_string(runIndex))
                                            .text(runText)
                                            .fontSize(lineFontSize)
                                            // 段没有自带字体（语法 token 着色段）→ 行级字体（代码块等宽）→ 控件字体。
                                            .fontFamily(!run.style.fontFamily.empty() ? run.style.fontFamily
                                                         : (!line.fontFamily.empty() ? line.fontFamily
                                                                                     : fontFamily_))
                                            .fontWeight(run.style.weight > 0 ? run.style.weight : 400)
                                            .lineHeight(textBand.height)
                                            .color(runColor)
                                            .wrap(false)
                                            .verticalAlign(core::VerticalAlign::Top)
                                            .build();
                                        if (run.style.strike) {
                                            // 删除线落在字面中部（同底色块，按字号定位而不是按行高）。
                                            ui_.rect(runId + ".strike")
                                                .position(run.x, textOrigin + lineFontSize * 0.46f)
                                                .size(std::max(1.0f, run.width), std::max(1.0f, lineFontSize * 0.07f))
                                                .color(runColor)
                                                .build();
                                        }
                                        if (run.style.underline) {
                                            ui_.rect(runId + ".underline")
                                                .position(run.x, textOrigin + lineFontSize * 0.88f)
                                                .size(std::max(1.0f, run.width), std::max(1.0f, lineFontSize * 0.06f))
                                                .color(runColor)
                                                .build();
                                        }
                                    }
                                }
                                // ── 块级图片（S3f 批次 B）──
                                // 文字已在装饰层整行藏进洞里；图的宽高也是装饰层按文件头
                                // 算好的，这里只按几何表给的行框原位画（行高 = 图高 + 留白，
                                // 垂直居中）。只画 lineStart 段：软换行的续行不重复画。
                                if (line.lineStart && !line.imagePath.empty() &&
                                    line.imageWidth > 0.0f && line.imageHeight > 0.0f) {
                                    const float imageY = y + (linePixelHeight - line.imageHeight) * 0.5f;
                                    const std::string imageId = id_ + ".img." + std::to_string(index);
                                    ui_.image(imageId)
                                        .position(0.0f, imageY)
                                        .size(line.imageWidth, line.imageHeight)
                                        .dirtyKey(textDirtyKey + "|I" + std::to_string(index))
                                        .source(line.imagePath)
                                        .radius(12.0f)
                                        .contain()
                                        .build();
                                    // 外框（ZCode markdown-image 的 border + rounded-xl）。
                                    // 画在图之后：1px 边线压在图的最外圈上。
                                    ui_.rect(imageId + ".frame")
                                        .position(0.0f, imageY)
                                        .size(line.imageWidth, line.imageHeight)
                                        .color(core::Color{0.0f, 0.0f, 0.0f, 0.0f})
                                        .border(1.0f, style_.border)
                                        .radius(12.0f)
                                        .build();
                                }
                                // ── 图片失败态占位盒（S3f 批次 F）──
                                // 本地路径解析失败/文件缺失/头解析失败：ZCode markdown-image
                                // 失败态的盒子（rounded-xl + 1px 边框 + 微底色），图标在
                                // 上半部居中、路径文字在下部。不设 onImage 短路 —— 点占位盒
                                // 会把光标放进行里、活动块显示源码，方便改路径。
                                if (line.lineStart && line.imageFailed &&
                                    line.imageWidth > 0.0f && line.imageHeight > 0.0f) {
                                    const float boxY = y + (linePixelHeight - line.imageHeight) * 0.5f;
                                    const std::string boxId = id_ + ".imgfail." + std::to_string(index);
                                    ui_.rect(boxId)
                                        .position(0.0f, boxY)
                                        .size(line.imageWidth, line.imageHeight)
                                        .color(style_.focused)
                                        .border(1.0f, style_.border)
                                        .radius(12.0f)
                                        .build();
                                    vector_icon::drawImage(
                                        ui_, boxId + ".icon", 0.0f, boxY, line.imageWidth,
                                        line.imageHeight * 0.62f, style_.placeholder);
                                    ui_.text(boxId + ".path")
                                        .position(12.0f, boxY + line.imageHeight * 0.62f)
                                        .size(line.imageWidth - 24.0f, line.imageHeight * 0.30f)
                                        .dirtyKey(textDirtyKey + "|G" + std::to_string(index))
                                        .text(line.imageFailText)
                                        .fontSize(std::max(9.0f, lineFontSize * 0.8f))
                                        .color(style_.placeholder)
                                        .horizontalAlign(core::HorizontalAlign::Center)
                                        .verticalAlign(core::VerticalAlign::Center)
                                        .build();
                                }
                            }
                        
                            }).build();
} else {
                            ui_.text(id_ + ".text")
                                .position(-state.horizontalScroll, -state.verticalScroll)
                                .size(layout.visibleTextWidth, renderedTextHeight)
                                .dirtyKey(textDirtyKey)
                                .text(empty ? placeholder_ : display.text)
                                .fontSize(fontSize)
                                .fontFamily(fontFamily_)
                                .lineHeight(textLineHeight)
                                .color(empty ? style_.placeholder : style_.text)
                                .wrap(false)
                                .verticalAlign(multiline_ ? core::VerticalAlign::Top : core::VerticalAlign::Center)
                                .build();
                        }

                        if (focused && InputModel::caretVisible(caretBlink_, focused, state)) {
                            // 光标高度跟随光标所在行的字号（标题行更高，行高不再全局统一）。
                            ui_.rect(id_ + ".cursor")
                                .position(caretX - inset, caretY - textY)
                                .size(1.5f, caretHeight)
                                .color(style_.cursor)
                                .radius(1.0f)
                                .build();
                        }
                    })
                    .cursor(cursor_)
                    .build();
                if(multiline_ && !state.wordWrap && layout.textWidth > textWidth) {
                    const float maximum=std::max(0.0f,layout.textWidth-textWidth+fontSize);
                    const float thumb=std::clamp(textWidth*textWidth/(layout.textWidth+fontSize),std::min(24.0f,textWidth),textWidth);
                    const float travel=std::max(0.0f,textWidth-thumb);
                    const float y=height_-10.0f;
                    ui_.rect(id_+".hscroll.track").position(inset,y).size(textWidth,8).radius(4).color(scrollStyle_.track)
                        .preserveFocusOnPress().onPress([&state,maximum,travel,thumb,textWidth](const core::PointerEvent& e,const core::Rect& b) {
                            const float scale=b.width/std::max(1.0f,textWidth);state.followCaret=false;
                            state.horizontalScroll=travel>0?std::clamp((static_cast<float>(e.x-b.x)/scale-thumb*.5f)/travel,0.0f,1.0f)*maximum:0;
                        }).build();
                    ui_.rect(id_+".hscroll.thumb").position(inset+(maximum>0?travel*state.horizontalScroll/maximum:0),y).size(thumb,8)
                        .radius(4).states(scrollStyle_.thumb,scrollStyle_.thumbHover,scrollStyle_.thumbPressed).cursor(core::CursorShape::Hand)
                        .preserveFocusOnPress().onPress([&state,thumb](const core::PointerEvent&,const core::Rect& b) {
                            state.followCaret=false;state.scrollbarDragOffset=state.horizontalScroll;state.scrollbarDragScale=std::max(.001f,b.width/thumb);
                        }).onDrag([&state,maximum,travel](const core::dsl::DragEvent& e) {
                            state.followCaret=false;if(travel>0) state.horizontalScroll=std::clamp(state.scrollbarDragOffset+static_cast<float>(e.totalX)/state.scrollbarDragScale*maximum/travel,0.0f,maximum);
                        }).build();
                }
                // 缩略图高度按"视口 : 内容"算，并夹在 [min(24, 视口高), 视口高] 里。
                // 先算再判：滑块高 == 轨道高时它一格也挪不动（视口比一行还矮的退化工况，
                // 例如高度 12px 的控件），此时整条滚动条不画 —— 画了也只能表达"没有位置
                // 信息"。这条判据原来由"纵向内缩后 textHeight 变成负数"隐式兜住。
                const float thumbHeight = textHeight > 0.0f
                    ? std::clamp(textHeight * textHeight / std::max(0.001f, layout.contentHeight),
                                 std::min(24.0f, textHeight), textHeight)
                    : 0.0f;
                if (scrollbarWidth > 0.0f && textHeight > 0.0f && thumbHeight < textHeight &&
                    layout.maxVerticalScroll > 0.0f) {
                    const float travel = textHeight - thumbHeight;
                    const float maximum = layout.maxVerticalScroll;
                    const float thumbY = travel * state.verticalScroll / maximum;
                    // 滚动条紧贴控件右缘（不留内缩）：右侧留白只服务文本（textWidth 里
                    // 已经扣掉 scrollbarGutter），滚动条本身不该跟着文本一起内缩。
                    const float barX = width_ - scrollbarWidth;
                    const auto wheel = [&state, maximum, fontSize](const core::ScrollEvent& event) {
                        state.followCaret = false;
                        state.verticalScroll = std::clamp(state.verticalScroll - static_cast<float>(event.y) *
                            std::max(12.0f, fontSize * 2.2f), 0.0f, maximum);
                    };
                    ui_.rect(id_ + ".scrollbar.track")
                        .position(barX, textViewportTop).size(scrollbarWidth, textHeight)
                        .color(scrollStyle_.track).radius(scrollStyle_.radius)
                        .preserveFocusOnPress().onScroll(wheel)
                        .onPress([&state, maximum, travel, thumbHeight, textHeight](const core::PointerEvent& event, const core::Rect& bounds) {
                            const float scale = bounds.height / textHeight;
                            const float y = static_cast<float>(event.y - bounds.y) / std::max(0.001f, scale);
                            state.followCaret = false;
                            state.verticalScroll = travel > 0.0f ? std::clamp((y - thumbHeight * 0.5f) / travel, 0.0f, 1.0f) * maximum : 0.0f;
                        }).build();
                    ui_.rect(id_ + ".scrollbar.thumb")
                        .position(barX, textViewportTop + thumbY).size(scrollbarWidth, thumbHeight)
                        .states(scrollStyle_.thumb, scrollStyle_.thumbHover, scrollStyle_.thumbPressed)
                        .radius(scrollStyle_.radius).cursor(core::CursorShape::Hand)
                        .preserveFocusOnPress().onScroll(wheel)
                        .onPress([&state, thumbHeight](const core::PointerEvent&, const core::Rect& bounds) {
                            state.followCaret = false;
                            state.scrollbarDragOffset = state.verticalScroll;
                            state.scrollbarDragScale = std::max(0.001f, bounds.height / thumbHeight);
                        })
                        .onDrag([&state, travel, maximum](const core::dsl::DragEvent& event) {
                            state.followCaret = false;
                            if (travel > 0.0f) {
                                state.verticalScroll = std::clamp(state.scrollbarDragOffset +
                                    static_cast<float>(event.totalY) / state.scrollbarDragScale * maximum / travel, 0.0f, maximum);
                            }
                        }).build();
                }
            })
            .build();
    }

private:
    using InputModel = input_detail::InputModel;
    using InputState = InputModel::InputState;
    using InputLayout = InputModel::InputLayout;

    core::dsl::Ui& ui_;
    std::string id_;
    InputStyle style_;
    ScrollStyle scrollStyle_;
    theme::ThemeMetricTokens metrics_;
    core::Transition transition_ = core::Transition::make(0.16f, core::Ease::OutCubic);
    std::function<void(const std::string&)> onChange_;
    std::function<void()> onImagePaste_;
    std::function<void()> onEnter_;
    std::function<void()> onEscape_;
    std::function<void(bool)> onFocus_;
    std::function<void(const InputModel::PointerHit&)> onPointerHit_;
    std::function<void(int)> onRevealHiddenLine_;
    std::function<void(float, float)> onContextMenu_;
    input_detail::LineDecorationProviderEx decorator_;
    input_detail::LineDecorationSnapshotProvider snapshotDecorator_;
    bool viewportMetrics_ = false;
    std::string text_;
    std::string placeholder_ = "Hello EUI-NEO 😉";
    bool multiline_ = false;
    bool wordWrap_ = true;
    bool caretBlink_ = false;
    core::CursorShape cursor_ = core::CursorShape::IBeam;
    bool scrollbar_ = false;
    float width_ = 260.0f;
    float height_ = 44.0f;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float inset_ = -1.0f;
    float maxTextWidth_ = 0.0f;
    bool lineNumbers_ = false;
    core::Color lineNumberColor_{0.0f, 0.0f, 0.0f, 0.0f};
    float fontSize_ = 0.0f;
    std::string fontFamily_ = "Microsoft YaHei";
    bool hasX_ = false;
    bool hasY_ = false;
};

inline InputBuilder input(core::dsl::Ui& ui, const std::string& id) {
    return InputBuilder(ui, id);
}

} // namespace components
