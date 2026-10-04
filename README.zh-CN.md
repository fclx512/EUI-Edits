# EUI-Edits

简体中文 · [English](README.md)

EUI-Edits 是一个主打低占用与现代 UI 的 Windows 原生记事本。保持简单文本编辑的同时，提供类似 Obsidian 的 Markdown 即时渲染与原位编辑体验，让格式随输入更新，阅读和编辑始终在同一视图中。

## 界面预览

![浅色主题下的 Markdown 即时渲染与原位编辑](docs/screenshots/markdown-light.png)

![深色主题下的 JSON 编辑与文档库](docs/screenshots/code-dark.png)

Markdown、文本、代码、数据和其他文件使用一套协调的自绘图标：

<p align="center">  
  <img src="apps/neo_editor/assets/icons/md.svg" width="56" alt="Markdown 文档图标">  
  \&nbsp;\&nbsp;  
  <img src="apps/neo_editor/assets/icons/txt.svg" width="56" alt="文本文件图标">  
  \&nbsp;\&nbsp;  
  <img src="apps/neo_editor/assets/icons/code.svg" width="56" alt="代码文件图标">  
  \&nbsp;\&nbsp;  
  <img src="apps/neo_editor/assets/icons/data.svg" width="56" alt="数据文件图标">  
  \&nbsp;\&nbsp;  
  <img src="apps/neo_editor/assets/icons/file.svg" width="56" alt="通用文件图标">  
</p>

<details>

<summary>更多截图：外观设置与文件关联</summary>

![英文界面的外观设置](docs/screenshots/settings-en.png)

![文件关联设置](docs/screenshots/associations-zh.png)

</details>

## 编辑体验

- **边写边看 Markdown。** 常见标题、强调、链接、列表、任务列表、引用、代码块和表格会在原文中即时更新。当前编辑区域显示 Markdown 源文，阅读和编辑不必切换文档。
- **本地文档库。** 默认浏览完整文件列表，包括隐藏文件和无扩展名文件。扫描只读取文件和目录元数据。
- **简单编辑各类文本。** 可打开 Markdown、纯文本、源码和数据文件。已识别格式提供基础语法着色，其他文本保持纯文本显示。

界面以 C++、Win32 和 Direct2D 构建。按需刷新，不嵌入 WebView。打开、保存、查找替换、撤销重做、行号和自动换行等常用记事本功能都在手边。图片预览支持滚轮缩放和中键拖拽平移，左键单击或 `Esc` 退出。

EUI-Edits 专注于本地文件浏览和简单编辑，不做知识库管理，也不提供代码补全、调试等 IDE 功能；程序不会执行脚本。

## 开始使用

设置和恢复草稿存放在 `%APPDATA%\EUI-Edits`；程序免安装，但不采用便携设置模式。首次使用时，关联列表预选 TXT 和 Markdown；BAT、CMD、PowerShell、Python 等其他类型可按需勾选。应用选择后会将 EUI-Edits 加入 Windows“打开方式”，但不会更改 Windows 默认应用；默认应用需另行在 Windows 设置中选择。

## 开发

在 Windows 上运行翻译和正确性检查：[`scripts/check-neoeditor.ps1`](scripts/check-neoeditor.ps1)。

## 当前范围

发布配置面向 **Windows 10/11 x64**。0.1.0 候选版已在 Windows 11 上检查，Windows 10 尚未完成同等范围的实机测试。程序目前未签名。

- 默认打开上限为 **64 MiB**。疑似二进制或无法可靠解码的内容会被拒绝。支持 UTF-8、UTF-16 和 Windows ANSI 编码。
- Markdown 聚焦于常见语法；公式、远程图片和演示模式不在当前范围内。
- 文档库首次扫描同步进行，特别大的目录可能需要等待。

## 许可与致谢

EUI-Edits 基于 [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) 开发。仓库保留框架源码，框架官方文档见 [EUI-NEO 仓库](https://github.com/sudoevolve/EUI-NEO)。

项目遵循仓库的 [Apache-2.0 许可](LICENSE)。应用主图标为项目重绘资源，并提供 16px 专用设计。第三方库和字体保留各自许可与署名，详见[第三方说明](apps/neo_editor/THIRD-PARTY-NOTICES.md)及 [LICENSES](apps/neo_editor/LICENSES)。字体处理使用了 FreeType Team 的工作。
