# EUI-Edits

EUI-Edits 是面向 Windows 的轻量原生文本与 Markdown 编辑器。它以低占用为设计目标，采用现代 Windows 桌面界面；Markdown 支持实时渲染与原位编辑，提供基础记事、查找替换和文档库功能。

EUI-Edits is a lightweight native text and Markdown editor for Windows, designed for low resource use with a modern Windows interface. Markdown combines live rendering with in-place editing, alongside basic note editing, find and replace, and a document library.

## 使用 / Use

发行文件为单个 `EUI-Edits-<版本>-windows-x64.exe`，无需解压资源目录即可运行。可选的同名 `.exe.sha256` 文件用于核对该 EXE；没有校验文件时也可直接运行。应用图标、界面图标及许可证文本嵌入程序。许可证可在“设置 → 关于”中查看并导出，也可运行 `EUI-Edits-<版本>-windows-x64.exe --export-licenses [输出路径]`；省略路径时导出到当前目录的 `EUI-Edits-LICENSES.txt`。

The release is a single `EUI-Edits-<version>-windows-x64.exe`; it runs without an extracted asset directory. An adjacent `.exe.sha256` file may be provided to verify the executable; the app can also run without it. The application icon, UI icons, and license texts are embedded. View and export licenses in Settings → About, or run `EUI-Edits-<version>-windows-x64.exe --export-licenses [output-path]`; without a path, the file is written as `EUI-Edits-LICENSES.txt` in the current directory.

可从菜单打开文档或将文件拖入窗口。文档库支持 F2 重命名；名称冲突时会提供编号名称建议，确认后再重命名，避免覆盖已有文件。图片预览中滚轮缩放，中键拖动平移。查找栏打开时，Ctrl+G 跳到下一处，Ctrl+Shift+G 跳到上一处。

Open a document from the menu or drop it into the window. Press F2 in the document library to rename; on a name conflict, the app suggests a numbered filename and waits for confirmation before renaming, avoiding overwrites. In image preview, use the wheel to zoom and middle-drag to pan. With the find bar open, Ctrl+G goes to the next match and Ctrl+Shift+G to the previous one.

设置和恢复副本保存在当前用户的 `%APPDATA%/EUI-Edits`。文件关联由 EUI-Edits 设置选择；Windows 设置中的默认应用是单独管理的。移动程序后需重新登记文件关联。许可证、源码和开发文档位于源码仓库中。

Settings and recovery copies are stored in the current user's `%APPDATA%/EUI-Edits`. Choose file associations in EUI-Edits settings; Windows default-app selection is managed separately. Re-register associations after moving the application. Licenses, source, and development documentation are available in the source repository.

## 平台与限制 / Platform and limits

面向 Windows x64，使用 Win32 与 Direct2D；发行包的 MSVC 运行时静态链接。打开文件默认上限为 64 MiB，疑似二进制或无法可靠解码的文件会被拒绝。文档库初次加载在后台扫描目录与文件元数据，不预先读取正文；部分显式文件操作会同步刷新列表，大型目录可能需要等待。Markdown 覆盖常见语法，不包含完整 Obsidian 扩展；公式、远程图片和演示模式不在当前范围。

Targets Windows x64 and uses Win32 and Direct2D; the release package statically links the MSVC runtime. The default file-open limit is 64 MiB; suspected binary files and files that cannot be decoded reliably are rejected. Initial library loading scans directory and file metadata in the background without pre-loading document contents; some explicit file operations refresh the list synchronously and may take time for large directories. Markdown covers common syntax, not the full set of Obsidian extensions; formulas, remote images, and presentation mode are outside the current scope.

## 版本与发布 / Version and release

0.1.1 已发布。0.1.2 修正表格中中文、emoji 与软换行文本的选择背景，减少输入组件中的正文副本、行号统计和目录过滤临时字符串，并在关闭动画且菜单闭合时省去隐藏菜单内容构造。性能数字仅来自指定软件渲染与单文档条件，不代表整体 CPU 或输入到屏幕时延；OPT-001 高频滚轮停滞的根因仍未确认，低配设备、硬件 GPU、真实 IME 和多显示器 DPI 等覆盖仍有限。详见[发布说明](RELEASE-NOTES.md)。当前构建与检查入口见[仓库 README](../../README.md#开发)。本目录与 CMake 目标 `neo_editor` 保留内部标识，产品和发行文件使用 EUI-Edits 名称；[历史发布构建说明](../../docs/NeoEditor-发布构建说明-2026-10-02.md)中的旧名称和绝对路径仅作参考，当前打包配置以脚本为准。

Version 0.1.1 is released. Version 0.1.2 fixes selection backgrounds for Chinese text, emoji, and wrapped text in tables, reduces repeated document copies, line-number counting, and directory-filter temporary strings in the input component, and skips hidden menu content construction when animations are disabled and the menu is closed. Performance numbers apply to specified software-rendering, single-document conditions and do not represent overall CPU or input-to-screen latency. The cause of OPT-001 high-frequency wheel stalls remains unconfirmed, and coverage is limited for low-end devices, hardware GPU, real IME, and multi-monitor DPI. See the [release notes](RELEASE-NOTES.md). Current build and check commands are in the [repository README](../../README_EN.md#development). This folder and the CMake target `neo_editor` retain internal identifiers; the product and release files use EUI-Edits. Old names and absolute paths in the [historical release build guide](../../docs/NeoEditor-发布构建说明-2026-10-02.md) are references only; the scripts define the current package configuration.
