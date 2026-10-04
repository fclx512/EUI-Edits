# NeoEditor 主题导入

> **路径说明（2026-10-04）**：文中提到的 `参考/…` 本地采集目录（Obsidian 样式规格、测量 JSON、标本截图、probe-vault、验证脚本等）已随发版清理整体删除。这些引用仅说明当时的取证方法与数据出处，结论仍有效；如需复查请按文中流程重新采集。


## 入口与格式

设置 → 主题文件 → 选择主题文件，会立即打开系统文件选择窗口。已下载的 Obsidian 社区主题需先解压，再选择主题目录中的 `manifest.json`（同目录必须有 `theme.css`），也可以直接选择 `theme.css`；NeoEditor 主题选择 `theme.json`。取消文件选择后会进入面板内的主题库，可挑选 `%APPDATA%/NeoEditor/themes/` 中的 NeoEditor JSON、CSS 文件，以及含 `manifest.json` + `theme.css` 的子目录。文件选择只记住原路径，不会自动复制进主题库。

NeoEditor schema v1 保持兼容：`base` 指定唯一被覆盖的明暗侧，载入后外观切到该侧。schema v2 用独立的 `light`、`dark` 对象；至少有一侧，切换外观时按侧取色，缺失字段回退对应内置主题。`colors` 接受 28 个语义字段；v2 的 `typography` 额外支持三级标题字号与行高、标题和表格的段前距。旧版文件仍按 v1 校验，导出使用 v2。

## Obsidian CSS 子集

解析器只摄取 `:root` / `body` / `html` 以及独立的 `.theme-light`、`.theme-dark` 规则中的 `--变量`，兼容这两个主题选择器出现在逗号列表里。支持十六进制、`rgb(a)`、`hsl(a)`、同侧 `var()` 链；不执行 CSS 布局、普通选择器、`calc()`、`color-mix()` 或插件样式。无法求值的颜色不写入覆盖表，而是回退内置值。设置面板会显示明暗各自的 `n/28`，载入时的提示会给出未映射项总数；这些数字是已成功映射的语义色数量，不表示整个 Obsidian CSS 的兼容百分比。

导入失败（文件缺失、格式错误、明暗两侧都没有可用颜色等）会保留当前主题。单个主题文件上限 8 MiB。真实社区主题常依赖 Obsidian 应用默认 CSS 或插件变量，所以只导入该主题的 CSS 时覆盖数可能很少，这是当前子集的预期降级。

## 验证记录（2026-09-27）

- `theme_loader` 单测验证 v1 兼容、v2 明暗分离与回退、导出往返、manifest/CSS 三通道、`var()` 链与环、`calc()` 降级、Unicode 路径和加载失败不改现场。
- `参考/obsidian-style/app.css` 的明暗侧各映射 22/28，与离线 bridge 金标准逐字段一致。固定截图 `tests/assets/markdown/screenshots/obsidian-default-{dark,light}.png` 展示导入后的分隔线，活动行截图验证源码切换。
- [Minimal 官方仓库](https://github.com/kepano/obsidian-minimal) 的 `manifest.json` + `theme.css` 可载入；单独 CSS 映射亮 4/28、暗 2/28，与同样只输入该 CSS 的离线 bridge 一致。若先叠 Obsidian 默认 `app.css`，离线 bridge 得到亮 12/28、暗 8/28；NeoEditor 当前不打包这份基础 CSS。另用本机 Blue Topaz 实包验证亮 13/28、暗 14/28。
- Release 构建与 `theme_loader`、`lp_decorations` 单测通过。隔离 APPDATA 的应用实测显示 Minimal 主题名和亮 4/28、暗 2/28；主题库列出 Minimal，点击亮色后设置持久化为 `theme=1` 且界面随之切换。设置页首击现已实测直接打开标题为“选择主题文件”的系统文件窗口，取消后显示主题库；窗口中选取本地 Blue Topaz 的 `manifest.json` 后，`last_theme_file` 成功落盘。
