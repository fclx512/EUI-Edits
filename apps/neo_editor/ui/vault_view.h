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
    // 地址栏：原来是 16 的只读路径文字，现在要让出输入框的内边距。
    constexpr float kRootHeight = 24.0f;
    constexpr float kFilterHeight = 28.0f;
    constexpr float kGap = 6.0f;
    constexpr float kChooseWidth = 92.0f;
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
                                      height - 16.0f - tabsHeight - kHeaderHeight - kRootHeight -
                                          kFilterHeight - kGap * 4.0f);
    const float titleWidth = std::max(
        0.0f, inner - kChooseWidth - kGap - kRowIconWidth - 4.0f);

    // 每帧只算一次相对路径；放进行回调会变成“每行一次文件系统调用”。
    const std::string activeRelative = state.path.empty() ? std::string{} : vaultRelativePath(state, state.path);
    std::vector<std::string> openedRelative;
    for (const auto& tab : documentTabs(state)) {
        if (!tab.path.empty() && documentWithinRoot(tab.path, state.vaultRoot))
            openedRelative.push_back(vaultRelativePath(state, tab.path));
    }
    const std::string selectedRelative = state.vaultSelectedPath;
    const std::string contextRelative = state.vaultContextMenuOpen ? state.vaultContextPath : std::string{};
    const std::string rootLabel =
        state.vaultRoot.empty() ? std::string(i18n::tr("vault.no_document")) : textfile::fileName(state.vaultRoot);
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
                        .gap(kGap)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.stack("vault.head.icon").size(kRowIconWidth, kHeaderHeight).content([&] {
                                iconView(ui, "vault.head.folder", UiIcon::Folder, 0,
                                         (kHeaderHeight-kRowIconWidth)*.5f, kRowIconWidth, colors.iconFolder);
                            }).build();
                            ui.text("vault.head.title")
                                .size(titleWidth, kHeaderHeight)
                                .text(elideToWidth(rootLabel, titleWidth, metrics.vaultTitleFontSize))
                                .fontFamily(uiFontFamily(state))
                                .fontSize(metrics.vaultTitleFontSize)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .color(colors.text)
                                .build();
                            iconTextButton(ui, "vault.choose", UiIcon::ArrowLeft, i18n::tr("vault.choose_directory"), kChooseWidth,
                                           metrics.vaultFilterFontSize,
                                           [&state] { chooseVaultDirectory(state); }, uiFontFamily(state));
                        })
                        .build();

                    // 地址栏。原来这里只是一行只读路径，进深层目录只能一层层点开；
                    // 现在是输入框：粘贴路径回车就跳过去（目录=展开到它，文件=展开并打开）。
                    // 平时底色跟侧栏一致，看着仍像一行路径文字，聚焦才显出输入框。
                    components::InputStyle addressStyle(colors.tokens);
                    addressStyle.background = colors.panel;
                    addressStyle.focused = colors.panel;
                    addressStyle.border = transparentColor();
                    addressStyle.focusBorder = colors.rowHover;
                    addressStyle.text = state.vaultAddress.empty() ? colors.textMuted : colors.text;
                    addressStyle.placeholder = colors.textMuted;
                    addressStyle.cursor = colors.accent;
                    addressStyle.radius = 4.0f;
                    addressStyle.shadow = eui::Shadow{};
                    components::input(ui, "vault.address")
                        .size(inner, kRootHeight)
                        .value(state.vaultAddress)
                        .placeholder(i18n::tr("vault.path_placeholder"))
                        .fontSize(metrics.vaultPathFontSize)
                        .fontFamily(uiFontFamily(state))
                        .inset(6.0f)
                        .style(addressStyle)
                        .transition(quickTransition())
                        .onChange([&state](const std::string& value) { state.vaultAddress = value; })
                        .onEnter([&state] { navigateToVaultPath(state, state.vaultAddress); })
                        .build();

                    components::input(ui, "vault.filter")
                        .size(inner, kFilterHeight)
                        .value(state.filter)
                        .placeholder(i18n::tr("vault.filter"))
                        .fontSize(metrics.vaultFilterFontSize)
                        .inset(8.0f)
                        .theme(colors.tokens)
                        .transition(quickTransition())
                        .onChange([&state](const std::string& value) {
                            state.filter = value;
                            rebuildRows(state);
                            state.vaultScroll = 0.0f;
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
        })
        .build();
}

} // namespace neo
