# EUI-Edits

EUI-Edits 是 Windows 原生文本与 Markdown 编辑器，使用 C++、Win32 和 Direct2D。当前源码版本为 0.2.4，作为预发布提供设置分组、字体搜索、字号样张和按分类恢复默认值等改进；可下载版本见[发布页](https://github.com/fclx512/EUI-Edits/releases)，改动与适用范围见[发布说明](RELEASE-NOTES.md)。

EUI-Edits is a native text and Markdown editor for Windows, built with C++, Win32 and Direct2D. The current source version is 0.2.4, a pre-release with grouped settings, font search, size samples, and category-specific defaults. See the [releases page](https://github.com/fclx512/EUI-Edits/releases) for downloads and the [release notes](RELEASE-NOTES.md) for changes and scope.

## 使用 / Use

发行文件为单个 `EUI-Edits-<版本>-windows-x64.exe`，直接运行。资源与许可嵌入程序；许可可在“设置 → 关于”查看和导出，也可运行 `EXE --export-licenses [输出路径]`。设置和恢复副本保存在 `%APPDATA%/EUI-Edits/`。

Run the single `EUI-Edits-<version>-windows-x64.exe` directly. Resources and licenses are embedded. View or export licenses in Settings → About, or run `EXE --export-licenses [output-path]`. Settings and recovery copies are stored in `%APPDATA%/EUI-Edits/`.

菜单或拖入均可打开文本和图片。文档库分层、分批加载目录元数据，打开文件才读取正文；聚焦条目后按 F2 重命名。图片在编辑区的独立只读标签中查看，支持切换、关闭、重排及独立视角；底部提供缩小、比例、放大和适应窗口，滚轮中心缩放，左键平移超出画布的部分，Esc、Ctrl+W 或关闭按钮关闭当前图片标签。非 Markdown 语法模式的右键菜单只保留剪切、复制、粘贴和全选。在「设置 → 文件与系统」可添加 TXT 和 Markdown 打开方式，其他格式在「自定义文件类型」中选择。Windows 默认应用在系统设置中选择，返回后自动刷新。移动 EXE 后可在新位置启动并修复打开路径。

Open text or images from the menu or by dropping files into the window. The library loads directory metadata one level and one batch at a time, reading document contents when opened; press F2 on a focused entry to rename. Images open in independent read-only tabs inside the editor region, with switching, closing, reordering, and per-tab views. Bottom controls provide zoom out, scale, zoom in, and Fit window; the wheel zooms around the canvas center and left-drag pans overflowing areas. Esc, Ctrl+W, or the close button closes the current image tab. Non-Markdown syntax modes show only Cut, Copy, Paste, and Select all in the editor context menu. Add TXT and Markdown to Open with in Settings → Files & system, or choose other formats under Customize file types. Set defaults in Windows and return to see the updated status. Launch the EXE from its new location to repair the open path after moving it.

设置中的 win10 / win11 风格分别使用菜单动画、固定边框与阴影：win10 在固定裁剪窗内短滑入，行高亮即时切换并保留亮边；win11 短浮入并保留相邻高亮跟随。关闭动画时直接切换。

The win10 / win11 appearance setting selects menu motion, fixed borders and shadows: win10 slides briefly inside a fixed clip and switches row highlights immediately while retaining Reveal; win11 uses a short floating entrance with adjacent highlight motion. Turning animations off applies changes immediately.

详细操作与功能范围见[中文说明](../../README.md)和[English guide](../../README_EN.md)。

外观与编辑器设置按用途分组，各行按文字调整高度。界面字体在“外观 → 界面大小”，正文和代码字体在“编辑器 → 文字与排版”；字号设置旁提供随当前字体和字号更新的样张。

字体选择页支持名称/文件名搜索和清空筛选。外观与编辑器页分别恢复本分类的默认设置，确认时显示影响范围；文稿、最近文件与文件关联保留。

Appearance and Editor settings are grouped by purpose, with rows sized to fit their text. Find the interface font under Appearance → Interface size, and body and code fonts under Editor → Text & typography. Size controls have samples that update with the current fonts and sizes.

The font picker supports name and filename search with a clear button. Appearance and Editor each restore only their own defaults, with the affected settings shown before confirmation. Documents, recent files, and file associations are kept.

关闭应用时可选择「丢弃并退出」，一次放弃所有未保存的文稿和主题草稿。主题工作台的「换个起始色」和 HSV 调整会试览新候选，点选后才采用。

启动后约 5 秒检查正式版本，自动请求间隔至少 24 小时。新版提示可以关闭，也可以打开发布页自行下载。提示开关在「设置 → 关于」。

Choose **Discard & exit** to drop all unsaved document and theme changes at once. In the theme workspace, changing the seed or its HSV values previews a new palette. Select a candidate to adopt it.

The editor checks for a stable release about five seconds after launch, at most once every 24 hours. Dismiss the notice or open the release page to download manually. Toggle notices in **Settings → About**.

## 开发 / Development

构建、验证和文档同步见[开发与维护规范](../../docs/开发流程与交接规范.md)，命令参数见[构建说明](../../docs/NeoEditor-发布构建说明-2026-10-02.md)。本目录与 CMake 目标沿用 `neo_editor` 内部标识。

See the [development and maintenance guide](../../docs/开发流程与交接规范.md) and [build parameters](../../docs/NeoEditor-发布构建说明-2026-10-02.md) (Chinese). This folder and the CMake target retain the internal `neo_editor` identifier.
