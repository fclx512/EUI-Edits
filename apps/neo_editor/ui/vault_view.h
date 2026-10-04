#pragma once

#include "model/i18n.h"

#include "state/app_actions.h"
#include "platform/vault_rename.h"
#include "ui/metrics.h"
#include "ui/outline_view.h"
#include "ui/widgets.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

// 文档库侧栏。视觉与尺寸都从 UiMetrics 取（原先这里全是 13/11/12/26/13 的字面量，
// 设置里改字号侧栏一点不动——那是"字号收口没收干净"的最后一处）。
//
// 行高必须保持固定值：virtualList 的前提是等高行，改成自适应会毁掉虚拟滚动。
namespace neo {
namespace vault_detail {
// 文档库由监视线程实时更新。

inline std::string elideToMeasuredWidth(const std::string& text, float maxWidth,
                                        const std::string& fontFamily, float fontSize) {
    if (maxWidth <= 0.0f || text.empty()) return {};
    const auto measure = [&](const std::string& value) {
        return core::TextPrimitive::measureTextWidth(value, fontFamily, fontSize);
    };
    if (measure(text) <= maxWidth) return text;
    const std::string ellipsis = "\xE2\x80\xA6";
    const float ellipsisWidth = measure(ellipsis);
    if (ellipsisWidth > maxWidth) return {};
    std::string prefix;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        const std::size_t length = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2
            : (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;
        const std::size_t next = std::min(text.size(), index + length);
        std::string candidate = prefix + text.substr(index, next - index) + ellipsis;
        if (measure(candidate) > maxWidth) return prefix + ellipsis;
        prefix.append(text, index, next - index);
        index = next;
    }
    return text;
}

inline void toolbarTooltip(eui::Ui& ui, const std::string& id, const std::string& source,
                           const std::string& text, float anchorX, float toolbarBottom,
                           float panelWidth, float panelHeight, const EditorColors& colors,
                           const char* fontFamily, float fontSize) {
    constexpr float kPaddingX = 10.0f;
    constexpr float kPaddingY = 6.0f;
    constexpr float kVerticalGap = 4.0f;
    const float available = std::max(0.0f, panelWidth - kPaddingX * 2.0f);
    const float measured = core::TextPrimitive::measureTextWidth(text, fontFamily, fontSize);
    const float tooltipWidth = std::min(available, std::max(std::min(86.0f, available),
                                                            measured + kPaddingX * 2.0f));
    const float textWidth = std::max(1.0f, tooltipWidth - kPaddingX * 2.0f);
    core::TextStyle textStyle;
    textStyle.text = text;
    textStyle.fontFamily = fontFamily;
    textStyle.fontSize = fontSize;
    textStyle.maxWidth = textWidth;
    textStyle.wrap = true;
    textStyle.lineHeight = fontSize + 5.0f;
    const float textHeight = std::max(textStyle.lineHeight, core::TextPrimitive::measureTextSize(textStyle).y);
    const float tooltipHeight = textHeight + kPaddingY * 2.0f;
    const float x = std::clamp(anchorX - tooltipWidth * 0.5f, kPaddingX,
                               std::max(kPaddingX, panelWidth - tooltipWidth - kPaddingX));
    const float wantedY = toolbarBottom + kVerticalGap;
    const float y = std::clamp(wantedY, 8.0f, std::max(8.0f, panelHeight - tooltipHeight - 8.0f));
    const eui::Color background = colors.tokens.dark
        ? core::mixColor(colors.tokens.surface, core::Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.18f)
        : core::Color{1.0f, 1.0f, 1.0f, 0.96f};
    ui.stack(id).position(x, y).size(tooltipWidth, tooltipHeight).zIndex(2600)
        .hoverOpacityFrom(source)
        .content([&] {
            ui.rect(id + ".bg").fill().radius(7.0f).color(background)
                .border(1.0f, colors.border).build();
            ui.text(id + ".text").position(kPaddingX, kPaddingY).size(textWidth, textHeight)
                .text(text).fontFamily(fontFamily).fontSize(fontSize).lineHeight(textStyle.lineHeight)
                .color(colors.tokens.text).horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center).wrap(true).build();
        }).build();
}

inline std::string lowerExtension(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return {};
    }
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

inline bool isCodeExtension(const std::string& ext) {
    static const char* kExtensions[] = {
        "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "cs", "java", "rs", "go", "py", "lua",
        "js", "jsx", "ts", "tsx", "json", "xml", "html", "htm", "css", "scss", "sh", "bash", "bat",
        "cmd", "ps1", "yaml", "yml", "toml", "ini", "cfg", "conf", "cmake", "gradle", "sql", "glsl",
        "vert", "frag", "hlsl"};
    for (const char* candidate : kExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

// 文档库会列出所有文件，认不出的扩展名走 filetypes::detect 的 default 分支，
// 用通用文件图标。
inline bool isMarkdownExtension(const std::string& ext) {
    return ext == "md" || ext == "markdown" || ext == "mdx" || ext == "mkd";
}

inline bool isTextExtension(const std::string& ext) {
    return ext == "txt" || ext == "text" || ext == "log" || ext == "csv";
}

inline UiIcon rowIcon(const vault::Row& row) {
    if (row.isDir) {
        return row.expanded ? UiIcon::FolderOpen : UiIcon::Folder;
    }
    switch(filetypes::detect(row.name).category) {
    case filetypes::Category::Markdown:return UiIcon::MarkdownFile;
    case filetypes::Category::Text:return UiIcon::TextFile;
    case filetypes::Category::Code:return UiIcon::CodeFile;
    case filetypes::Category::Data:return UiIcon::DataFile;
    default:return UiIcon::File;
    }
}

inline eui::Color rowIconColor(const EditorColors& colors, const vault::Row& row) {
    if (row.isDir) {
        return colors.iconFolder;
    }
    switch(filetypes::detect(row.name).category) {
    case filetypes::Category::Markdown:return colors.iconMd;
    case filetypes::Category::Text:return colors.iconTxt;
    case filetypes::Category::Code:return colors.iconCode;
    case filetypes::Category::Data:return colors.iconData;
    default:return colors.iconFile;
    }
}

inline bool sameRowPath(const std::string& a, const std::string& b) {
    return platform::sameVaultRelativePath(a, b);
}
inline std::string stableRowId(const std::string& relative) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string id = "vault.entry";
    id.reserve(id.size() + relative.size() * 2);
    for (const unsigned char byte : relative) {
        id.push_back('.');
        id.push_back(hex[byte >> 4]);
        id.push_back(hex[byte & 0x0f]);
    }
    return id;
}

} // namespace vault_detail

inline void vaultPanelView(eui::Ui& ui, AppState& state, float width, float height) {
    if (state.vaultTab == VaultTab::Outline) {
        outlinePanelView(ui, state, width, height);
        return;
    }
    using namespace vault_detail;
    const EditorColors& colors = editorColors();
    const UiMetrics metrics = uiMetrics(state);

    constexpr float kPadding = 10.0f;
    constexpr float kHeaderHeight = 26.0f;
    constexpr float kRootHeight = 30.0f;
    constexpr float kGap = 6.0f;
    constexpr float kToolbarGap = 2.0f;
    constexpr float kModeButtonWidth = 28.0f;
    constexpr float kRowIconWidth = 16.0f;
    constexpr float kCaretWidth = 13.0f;
    constexpr float kIconGap = 6.0f;

    const float rowHeight = metrics.vaultRowHeight;
    const float tabsHeight = metrics.vaultTitleFontSize + 20.0f;
    const float inner = std::max(0.0f, width - kPadding * 2.0f);

    // 地址栏跳转后把目标行滚进视野。行号要等 rows 重建完才存在，
    // 所以跳转那一帧只记了相对路径，这里消费。
    if (!state.vaultPendingReveal.empty()) {
        for (std::size_t i = 0; i < state.rows.size(); ++i) {
            if (state.rows[i].relative == state.vaultPendingReveal) {
                // 留两行余量，目标不会贴在列表顶端。
                state.vaultScroll = std::max(0.0f, static_cast<float>(i) * rowHeight - rowHeight * 2.0f);
                break;
            }
        }
        state.vaultPendingReveal.clear();
    }
    const float listHeight = std::max(0.0f,
                                      height - 16.0f - tabsHeight - kHeaderHeight - kRootHeight - kGap * 3.0f);
    // Give the action its measured label width while keeping it inside the sidebar.
    const float chooseGlyph = std::min(18.0f, metrics.vaultFilterFontSize + 2.0f);
    const float chooseNaturalWidth = core::TextPrimitive::measureTextWidth(
        i18n::tr("vault.choose_directory"), uiFontFamily(state), metrics.vaultFilterFontSize) + chooseGlyph + 18.0f;
    const float chooseAvailable = std::max(0.0f, inner - kModeButtonWidth * 2.0f - kToolbarGap * 3.0f);
    const float chooseWidth = std::min(chooseAvailable, std::max(82.0f, chooseNaturalWidth));
    const std::filesystem::path currentVaultPath = state.vaultRoot.empty()
        ? std::filesystem::path{} : textfile::pathFromUtf8(state.vaultRoot).lexically_normal();
    const std::filesystem::path vaultParent = currentVaultPath.parent_path();
    const bool canGoParent = !currentVaultPath.empty() && !vaultParent.empty() && vaultParent != currentVaultPath;

    // 每帧只算一次相对路径；放进行回调会变成“每行一次文件系统调用”。
    const std::string activeRelative = state.path.empty() ? std::string{} : vaultRelativePath(state, state.path);
    std::vector<std::string> openedRelative;
    for (const auto& tab : documentTabs(state)) {
        if (!tab.path.empty() && documentWithinRoot(tab.path, state.vaultRoot))
            openedRelative.push_back(vaultRelativePath(state, tab.path));
    }
    const std::string selectedRelative = state.vaultSelectedPath;
    const std::string contextRelative = state.vaultContextMenuOpen ? state.vaultContextPath : std::string{};
    const bool empty = state.rows.empty();

    ui.stack("vault")
        .size(width, height)
        .content([&] {
            ui.rect("vault.bg")
                .fill()
                .color(colors.panel)
                .build();

            ui.column("vault.content")
                .fill()
                .padding(kPadding, 8.0f)
                .gap(kGap)
                .zIndex(1)
                .content([&] {
                    vaultTabsView(ui, state, inner, metrics.vaultTitleFontSize);
                    ui.row("vault.head")
                        .size(inner, kHeaderHeight)
                        .gap(kToolbarGap)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            const bool filterMode = !state.vaultPathInputMode;
                            const eui::Color activeModeFill = rgba(colors.accent.r, colors.accent.g,
                                                                  colors.accent.b, 0.14f);
                            ui.stack("vault.mode.filter").size(kModeButtonWidth, kHeaderHeight).content([&] {
                                ui.rect("vault.mode.filter.hit").fill().radius(5.0f)
                                    .color(filterMode ? activeModeFill : transparentColor())
                                    .border(filterMode ? 1.0f : 0.0f, colors.accent)
                                    .states(filterMode ? activeModeFill : transparentColor(), colors.rowHover,
                                            colors.pressed)
                                    .cursor(eui::CursorShape::Hand)
                                    .onClick([&state, &ui] {
                                        // A shared field must not undo into the other mode, even
                                        // when both retained values happen to be identical.
                                        auto& input = ui.state<components::input_detail::InputModel::InputState>("vault.address");
                                        input.undoStack.clear();
                                        input.redoStack.clear();
                                        state.vaultPathInputMode = !state.vaultPathInputMode;
                                        rebuildRows(state);
                                        state.vaultScroll = 0.0f;
                                        app::requestUpdate();
                                    }).build();
                                iconView(ui, "vault.mode.filter.icon", UiIcon::Search,
                                         (kModeButtonWidth - 18.0f) * 0.5f,
                                         (kHeaderHeight - 18.0f) * 0.5f, 18.0f,
                                         filterMode ? colors.accent : colors.textMuted);
                            }).build();
                            ui.stack("vault.parent").size(kModeButtonWidth, kHeaderHeight).content([&] {
                                auto hit = ui.rect("vault.parent.hit").fill().radius(5.0f)
                                    .states(transparentColor(), colors.rowHover, colors.pressed)
                                    .preserveFocusOnPress().disabled(!canGoParent);
                                if (canGoParent) {
                                    hit.cursor(eui::CursorShape::Hand).onClick([&state] {
                                        const std::filesystem::path current =
                                            textfile::pathFromUtf8(state.vaultRoot).lexically_normal();
                                        const std::filesystem::path parent = current.parent_path();
                                        if (parent.empty() || parent == current) return;
                                        state.vaultRoot = textfile::pathToUtf8(parent);
                                        state.vaultAddress = state.vaultRoot;
                                        state.vaultScan.reset();
                                        state.vaultRowsGeneration = 0;
                                        state.expanded.clear();
                                        refreshVault(state, true);
                                        app::requestUpdate();
                                    });
                                }
                                hit.build();
                                const float glyph = 18.0f;
                                iconView(ui, "vault.parent.icon", UiIcon::Previous,
                                         (kModeButtonWidth-glyph)*.5f, (kHeaderHeight-glyph)*.5f,
                                         glyph, canGoParent ? colors.text : colors.textMuted);
                            }).build();
                            ui.stack("vault.head.spacer").fill().build();
                            const float chooseTextWidth = std::max(
                                0.0f, chooseWidth - std::min(18.0f, metrics.vaultFilterFontSize + 2.0f) - 18.0f);
                            iconTextButton(ui, "vault.choose", UiIcon::FolderOpen,
                                           elideToMeasuredWidth(i18n::tr("vault.choose_directory"), chooseTextWidth,
                                                                uiFontFamily(state), metrics.vaultFilterFontSize),
                                           chooseWidth,
                                           metrics.vaultFilterFontSize,
                                           [&state] { chooseVaultDirectory(state); }, uiFontFamily(state));
                        })
                        .build();

                    // 路径跳转和文件筛选共用一个输入框，切换时各自的文本都保留。
                    components::InputStyle unifiedStyle(colors.tokens);
                    unifiedStyle.background = colors.panel;
                    unifiedStyle.focused = colors.panel;
                    unifiedStyle.border = colors.border;
                    unifiedStyle.focusBorder = colors.accent;
                    unifiedStyle.text = (state.vaultPathInputMode ? state.vaultAddress : state.filter).empty()
                        ? colors.textMuted : colors.text;
                    unifiedStyle.placeholder = colors.textMuted;
                    unifiedStyle.cursor = colors.accent;
                    unifiedStyle.radius = 5.0f;
                    unifiedStyle.shadow = eui::Shadow{};
                    components::input(ui, "vault.address")
                        .size(inner, kRootHeight)
                        .value(state.vaultPathInputMode ? state.vaultAddress : state.filter)
                        .placeholder(i18n::tr(state.vaultPathInputMode
                                                  ? "vault.path_placeholder" : "vault.filter"))
                        .fontSize(metrics.vaultFilterFontSize)
                        .fontFamily(uiFontFamily(state))
                        .inset(8.0f)
                        .style(unifiedStyle)
                        .transition(quickTransition())
                        .onChange([&state](const std::string& value) {
                            if (state.vaultPathInputMode) {
                                state.vaultAddress = value;
                            } else {
                                state.filter = value;
                                rebuildRows(state);
                                state.vaultScroll = 0.0f;
                            }
                        })
                        .onEnter([&state] {
                            if (state.vaultPathInputMode) navigateToVaultPath(state, state.vaultAddress);
                        })
                        .build();

                    if (!state.vaultRoot.empty() && !state.vaultScan) {
                        // 首次尚无共享快照：明确加载态（扫描在后台进行），不把加载中当错误。
                        ui.text("vault.loading")
                            .size(inner, 24.0f)
                            .text(i18n::tr("vault.loading"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultFilterFontSize)
                            .color(colors.textMuted)
                            .wrap(true)
                            .build();
                        return;
                    }
                    if (!state.vaultRoot.empty() && state.vaultScan && !state.vaultScan->ok) {
                        ui.text("vault.unavailable.title")
                            .size(inner, 24.0f)
                            .text(i18n::tr("vault.offline"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultFilterFontSize)
                            .color(colors.text)
                            .wrap(true)
                            .build();
                        ui.text("vault.unavailable.error")
                            .size(inner, 52.0f)
                            .text(state.vaultScan->error.empty() ? i18n::tr("vault.current_unreadable")
                                                                : state.vaultScan->error)
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultPathFontSize)
                            .color(colors.textMuted)
                            .wrap(true)
                            .build();
                        iconTextButton(ui, "vault.relocate", UiIcon::ArrowLeft, i18n::tr("vault.relocate"), inner,
                                       metrics.vaultFilterFontSize,
                                       [&state] { chooseVaultDirectory(state); }, uiFontFamily(state));
                        return;
                    }

                    if (empty) {
                        ui.text("vault.empty")
                            .size(inner, 40.0f)
                            .text(state.vaultRoot.empty() ? i18n::tr("vault.open_hint")
                                                          : i18n::tr("vault.no_matches"))
                            .fontFamily(uiFontFamily(state))
                            .fontSize(metrics.vaultFilterFontSize)
                            .color(colors.textMuted)
                            .wrap(true)
                            .build();
                        return;
                    }

                    components::virtualList(ui, "vault.list")
                        .theme(colors.tokens)
                        .size(inner, listHeight)
                        .itemCount(static_cast<std::int64_t>(state.rows.size()))
                        .rowHeight(rowHeight)
                        .offset(state.vaultScroll)
                        .step(78.0f)
                        .overscanViewports(0.5f)
                        .transition(quickTransition())
                        .onChange([&state](float value) { state.vaultScroll = value; })
                        // kCaretWidth / kRowIconWidth / kIconGap 是函数内的 constexpr，
                        // 显式捕获列表不会隐式带上它们，必须逐个列出来。
                        .row([&state, &colors, &metrics, activeRelative, openedRelative, selectedRelative, contextRelative, rowHeight,
                              kCaretWidth, kRowIconWidth, kIconGap](eui::Ui& rowUi,
                                                                    const std::string&,
                                                                    std::int64_t index,
                                                                    float rowWidth,
                                                                    float) {
                            const auto rowIndex = static_cast<std::size_t>(index);
                            if (rowIndex >= state.rows.size()) {
                                return;
                            }
                            const vault::Row row = state.rows[rowIndex];
                            const std::string absolute =
                                row.isDir ? std::string{} : vaultAbsolutePath(state, row.relative);
                            const bool active =
                                !row.isDir && !activeRelative.empty() && sameRowPath(row.relative, activeRelative);
                            const bool opened = std::any_of(openedRelative.begin(), openedRelative.end(), [&](const std::string& path) {
                                return sameRowPath(row.relative, path) || (row.isDir && path.rfind(row.relative + "/", 0) == 0);
                            });
                            const bool selected = row.relative == selectedRelative || row.relative == contextRelative;
                            // 缩进 + 折叠箭头 + 类型图标 + 文字四段固定宽度，行与行之间图标才对得齐。
                            // 文件没有箭头，但也留出这段宽度，图标列才不会被文件行顶歪。
                            const float indent =
                                4.0f + static_cast<float>(row.depth) * metrics.vaultIndentPerDepth;
                            const float labelX = indent + kCaretWidth + kRowIconWidth + kIconGap;
                            const float labelWidth = std::max(0.0f, rowWidth - labelX - (opened ? 24.0f : 12.0f));

                            const eui::Color selectedFill = core::mixColor(colors.panel, colors.accent, 0.12f);
                            const eui::Color normal = active ? colors.rowActive
                                                            : (selected ? selectedFill : transparentColor());
                            const eui::Color hover = active ? colors.rowActive
                                                           : (selected ? selectedFill : colors.rowHover);
                            const std::string stableId = stableRowId(row.relative);

                            if (state.vaultFocusRestorePath == row.relative) {
                                rowUi.requestFocus(stableId + ".bg");
                                state.vaultFocusRestorePath.clear();
                            }
                            rowUi.stack(stableId)
                                .size(rowWidth, rowHeight)
                                .content([&] {
                                    rowUi.rect(stableId + ".bg")
                                        .fill()
                                        .radius(4.0f)
                                        .states(normal, hover, colors.pressed)
                                        .transition(quickTransition())
                                        .animate(core::AnimProperty::Color | core::AnimProperty::Border)
                                        .border(selected && !active ? 1.0f : 0.0f,
                                                selected && !active
                                                    ? components::theme::withAlpha(colors.accent, 0.36f)
                                                    : transparentColor())
                                        .cursor(eui::CursorShape::Hand)
                                        .focusable()
                                        .onFocusChanged([&state, row](bool focused) {
                                            if (focused) {
                                                state.vaultRowFocusedPath = row.relative;
                                                state.vaultSelectedPath = row.relative;
                                            } else if (state.vaultRowFocusedPath == row.relative) state.vaultRowFocusedPath.clear();
                                        })
                                        .onKeyEvent([&state, row](const eui::KeyEvent& event) {
                                            if (event.isDown() && event.key == eui::InputKey::F2 &&
                                                !event.modifiers.shortcut() && !event.modifiers.alt && !event.modifiers.shift) {
                                                beginVaultRename(state, row.relative, row.isDir, true); return true;
                                            }
                                            return false;
                                        })
                                        .onClick([&state, row, absolute, active] {
                                            state.vaultSelectedPath = row.relative;
                                            if (row.isDir) {
                                                toggleFolder(state, row.relative);
                                            } else if (!active) {
                                                requestOpenPath(state, absolute);
                                            }
                                        })
                                        // 右键（2026-09-26）：记住目标行并弹菜单
                                        // （本体是 vaultContextMenuOverlay，app.cpp）。
                                        .onContextMenu([&state, row](const core::PointerEvent& event,
                                                                     const core::Rect&) {
                                            state.vaultSelectedPath = row.relative;
                                            state.vaultContextPath = row.relative;
                                            state.vaultContextIsDir = row.isDir;
                                            state.vaultContextMenuX = static_cast<float>(event.x);
                                            state.vaultContextMenuY = static_cast<float>(event.y);
                                            state.vaultContextMenuOpen = true;
                                            app::requestUpdate();
                                        })
                                        .build();

                                    // 当前文件左侧的主色条：比只把文字换色更容易一眼找到。
                                    if (active || selected) {
                                        rowUi.rect(stableId + ".mark")
                                            .position(1.0f, 4.0f)
                                            .size(active ? 2.0f : 1.0f, std::max(0.0f, rowHeight - 8.0f))
                                            .radius(1.0f)
                                            .color(active ? colors.accent
                                                          : components::theme::withAlpha(colors.accent, 0.62f))
                                            .transition(quickTransition())
                                            .animate(core::AnimProperty::Color)
                                            .build();
                                    }

                                    if (row.isDir) {
                                        iconView(rowUi, stableId + ".caret",
                                                 row.expanded ? UiIcon::ChevronDown : UiIcon::ChevronRight,
                                                 indent + (kCaretWidth-12.0f)*.5f,
                                                 (rowHeight-12.0f)*.5f, 12.0f, colors.textMuted);
                                    }
                                    iconView(rowUi, stableId + ".icon", rowIcon(row),
                                             indent + kCaretWidth, (rowHeight-16)*.5f, 16,
                                             rowIconColor(colors, row));

                                    if (opened) rowUi.rect(stableId + ".opened")
                                        .position(rowWidth - 15.0f, (rowHeight - 6.0f) * .5f)
                                        .size(6.0f, 6.0f).radius(3.0f)
                                        .color(active ? colors.accent : components::theme::withAlpha(colors.accent, .60f))
                                        .build();
                                    rowUi.text(stableId + ".label")
                                        .position(labelX, 0.0f)
                                        .size(labelWidth, rowHeight)
                                        .text(elideToWidth(row.name, labelWidth, metrics.vaultRowFontSize))
                                        .fontFamily(uiFontFamily(state))
                                        .fontSize(metrics.vaultRowFontSize)
                                        .color(active ? colors.accent : colors.text)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .transition(quickTransition())
                                        .build();
                                })
                                .build();
                        })
                        .build();
                })
                .build();
            const float toolbarCenterY = 8.0f + tabsHeight + kGap + kHeaderHeight * 0.5f;
            toolbarTooltip(ui, "vault.mode.tooltip", "vault.mode.filter.hit",
                           i18n::tr(state.vaultPathInputMode ? "vault.filter_mode" : "vault.path_mode"),
                           kPadding + kModeButtonWidth * 0.5f, toolbarCenterY + kHeaderHeight * 0.5f,
                           width, height, colors, uiFontFamily(state), metrics.vaultFilterFontSize);
            toolbarTooltip(ui, "vault.parent.tooltip", "vault.parent.hit",
                           i18n::tr("vault.parent_directory"),
                           kPadding + kModeButtonWidth + kToolbarGap + kModeButtonWidth * 0.5f,
                           toolbarCenterY + kHeaderHeight * 0.5f,
                           width, height, colors, uiFontFamily(state), metrics.vaultFilterFontSize);
            toolbarTooltip(ui, "vault.choose.tooltip", "vault.choose.hit",
                           i18n::tr("vault.choose_directory"),
                           kPadding + inner - chooseWidth * 0.5f, toolbarCenterY + kHeaderHeight * 0.5f,
                           width, height, colors, uiFontFamily(state), metrics.vaultFilterFontSize);
        })
        .build();
}

} // namespace neo
