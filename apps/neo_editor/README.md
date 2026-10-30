# EUI-Edits

EUI-Edits 是 Windows 原生文本与 Markdown 编辑器，使用 C++、Win32 和 Direct2D。当前源码版本为 0.2.3，可下载版本见[发布页](https://github.com/fclx512/EUI-Edits/releases)，本版变化见[发布说明](RELEASE-NOTES.md)。

EUI-Edits is a native text and Markdown editor for Windows, built with C++, Win32 and Direct2D. The current source version is 0.2.3. See the [releases page](https://github.com/fclx512/EUI-Edits/releases) for downloads and the [release notes](RELEASE-NOTES.md) for this version's changes.

## 使用 / Use

发行文件为单个 `EUI-Edits-<版本>-windows-x64.exe`，直接运行。资源与许可嵌入程序；许可可在“设置 → 关于”查看和导出，也可运行 `EXE --export-licenses [输出路径]`。设置和恢复副本保存在 `%APPDATA%/EUI-Edits/`。

Run the single `EUI-Edits-<version>-windows-x64.exe` directly. Resources and licenses are embedded. View or export licenses in Settings → About, or run `EXE --export-licenses [output-path]`. Settings and recovery copies are stored in `%APPDATA%/EUI-Edits/`.

菜单或拖入均可打开文本和图片。文档库分层、分批加载目录元数据，打开文件才读取正文；聚焦条目后按 F2 重命名。图片在编辑区的独立只读标签中查看，支持切换、关闭、重排及独立视角；底部提供缩小、比例、放大和适应窗口，滚轮中心缩放，左键平移超出画布的部分，Esc、Ctrl+W 或关闭按钮关闭当前图片标签。非 Markdown 语法模式的右键菜单只保留剪切、复制、粘贴和全选。文件关联在设置中登记，默认应用在 Windows 中选择；移动 EXE 后重新登记关联。

Open text or images from the menu or by dropping files into the window. The library loads directory metadata one level and one batch at a time, reading document contents when opened; press F2 on a focused entry to rename. Images open in independent read-only tabs inside the editor region, with switching, closing, reordering, and per-tab views. Bottom controls provide zoom out, scale, zoom in, and Fit window; the wheel zooms around the canvas center and left-drag pans overflowing areas. Esc, Ctrl+W, or the close button closes the current image tab. Non-Markdown syntax modes show only Cut, Copy, Paste, and Select all in the editor context menu. Register file associations in Settings and choose default apps in Windows; re-register after moving the EXE.

设置中的 win10 / win11 风格分别使用菜单动画、固定边框与阴影：win10 在固定裁剪窗内短滑入，行高亮即时切换并保留亮边；win11 短浮入并保留相邻高亮跟随。关闭动画时直接切换。

The win10 / win11 appearance setting selects menu motion, fixed borders and shadows: win10 slides briefly inside a fixed clip and switches row highlights immediately while retaining Reveal; win11 uses a short floating entrance with adjacent highlight motion. Turning animations off applies changes immediately.

详细操作与功能范围见[中文说明](../../README.md)和[English guide](../../README_EN.md)。

## 开发 / Development

构建、验证和文档同步见[开发与维护规范](../../docs/开发流程与交接规范.md)，命令参数见[构建说明](../../docs/NeoEditor-发布构建说明-2026-10-02.md)。本目录与 CMake 目标沿用 `neo_editor` 内部标识。

See the [development and maintenance guide](../../docs/开发流程与交接规范.md) and [build parameters](../../docs/NeoEditor-发布构建说明-2026-10-02.md) (Chinese). This folder and the CMake target retain the internal `neo_editor` identifier.
