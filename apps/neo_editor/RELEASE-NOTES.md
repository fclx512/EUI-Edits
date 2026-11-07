# EUI-Edits @NEO_EDITOR_VERSION@ — 发布说明

0.2.4 预发布，主要整理设置体验，并完善主题工作台、文件关联和更新提示。欢迎试用后反馈。

## 中文

### 设置与字体

- 外观、编辑器设置按用途分组，每行根据自己的文字和换行调整高度，窄窗口下也能完整查看说明。
- 界面字体放在「外观」，正文和代码字体放在「编辑器」。字号旁增加随当前字体、字号更新的实际大小样张。
- 字体选择页增加名称和文件名搜索、结果数量、清空筛选及无结果提示；支持用空格组合多个关键词。
- 「恢复默认值」只恢复当前的外观或编辑器分类，确认时列出影响范围；另一分类、文稿、最近文件和文件关联保留。

### 主题工作台

- 在完整样张中查看和调整配色，导入报告可转为独立主题草稿；支持明暗配色编辑、对比、撤销、重做和保存复用。
- 更换起始色或调整色相、饱和度、明度时先试览候选，点选后采用，并保留锁定色及导入色的处理规则。
- 修正明暗主题排版覆盖和保存后的基线恢复，缺省的三级标题行高保持默认值。
- CSS 导入按顶层选择器分别处理通用、浅色和深色颜色。导入范围仍为受支持的颜色变量，不执行 CSS 布局、插件或条件规则。

### 文件、关于与退出

- 文件关联页增加 TXT、Markdown 快捷入口；从 Windows 默认应用设置返回后自动刷新状态，移动 EXE 后可修复打开路径。
- 关于页整理版本、项目入口、文件类型和开源许可信息。
- 增加后台正式版更新提示，可打开发布页手动下载，在「设置 → 关于」关闭。自动查询间隔至少 24 小时；查询失败不打扰编辑，不自动安装更新，也不推送预发布版本。
- 退出确认增加「丢弃并退出」，一次放弃全部未保存文稿和主题草稿；取消则继续编辑。

### 下载与更新

下载 `EUI-Edits-0.2.4-windows-x64.exe`，关闭旧程序后直接运行新 EXE。设置和恢复副本仍位于 `%APPDATA%/EUI-Edits/`；移动 EXE 后可在文件关联页修复路径。发行包为未签名的 Windows 10/11 x64 单文件，附带 SHA256 校验文件。

本轮通过完整自动检查和候选包实窗验收；真实中文 IME、异机与多屏 DPI 场景未在本轮候选上复测。

## English

0.2.4 is a pre-release focused on settings, with improvements to the theme workspace, file associations, and update notices. Feedback is welcome.

### Settings and fonts

- Appearance and Editor settings are grouped by purpose. Each row fits its own text and wrapping, including in narrow windows.
- Interface fonts are under Appearance; body and code fonts are under Editor. Font-size controls now show samples at the selected font and size.
- The font picker supports name and filename search, result counts, clearing the filter, and an empty-results hint. Separate keywords with spaces to combine them.
- Restore defaults affects only the current Appearance or Editor category and shows its scope before confirmation. The other category, documents, recent files, and file associations are kept.

### Theme workspace

- Inspect and edit colors on a full sample, turn an import report into an independent theme draft, and compare, undo, redo, save, and reuse light and dark palettes.
- Changing the seed or its hue, saturation, and brightness previews a candidate before adoption, while respecting pinned and imported colors.
- Fix light/dark typography overrides and saved baselines; an unspecified level-three heading line height keeps its default.
- CSS import handles common, light, and dark colors by top-level selector. It imports supported color variables; CSS layout, plugins, and conditional rules are not executed.

### Files, About, and exit

- Add TXT and Markdown association shortcuts, refresh default-app status after returning from Windows settings, and repair the open path after moving the EXE.
- Simplify About information for the version, project, file types, and open-source licenses.
- Add background stable-release notices with a link for manual download and an off switch in Settings → About. Automatic checks are at least 24 hours apart; failures stay silent. Updates are not installed automatically, and pre-releases are excluded from these notices.
- Discard & exit drops all unsaved documents and theme drafts together. Cancel returns to editing.

### Download and update

Download `EUI-Edits-0.2.4-windows-x64.exe`, close the old app, and run the new EXE directly. Settings and recovery copies remain in `%APPDATA%/EUI-Edits/`. Repair file associations if you move the EXE. The unsigned release is a single Windows 10/11 x64 executable with a SHA256 checksum file.

The full automated checks and candidate GUI acceptance passed. Real Chinese IME, other machines, and mixed-DPI displays were not retested on this candidate.
