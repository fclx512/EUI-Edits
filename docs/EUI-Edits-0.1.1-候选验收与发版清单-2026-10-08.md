# EUI-Edits 0.1.1 候选验收与发版清单（2026-10-08）

> 后续授权：用户已明确要求“清理缓存防止误传无关内容，发版并提交”。发版整合沿用本页已验收候选；已清理 11 个构建/字节码缓存目录（约 1.84 GiB），构建诊断与 CTest 日志归档至 `out/release-0.1.1-20261008/cache-cleanup-evidence/`。本页其余内容保留候选验收时的历史状态；提交/推送/标签/公开发布执行结果保存在同输出目录的 publication 记录。

本次已整合前五批性能补丁、HEAD 中的表格/图片修复，以及三个可快速落地的补充。用户已确定版本 **0.1.1**。候选构建、正式门禁和本机基本交互验收完成；仍在 `main` / `44123ad8412010f01632763aa99824e7545bbbec` 的未提交工作树中。未 stage、提交、推送、打标签或公开发布。

## 本轮补充

1. **目录快照在后台准备。** `vault_cache.cpp` 的 worker 现在生成共享只读快照及字节估算；主线程回调发布引用，不再逐项估算，也避免 `core::async` 调用 `then(result)` 时复制整棵 `ScanResult`。同步刷新也在缓存锁外估算。代次、过期结果丢弃、同根请求合并和显式同步刷新语义保持原合同。新增 256 目录 / 32,768 文件嵌套快照回归，验证完整树、后台/同步估算一致性和字节预算淘汰。没有本轮总 CPU / 主线程耗时多轮 A/B，不给出加速倍数。
2. **正式正确性门禁由 47 扩为 51 项。** 加入 `input_model`、`lp_decorations`、`undo_incremental`、`image_attach`，覆盖已提交的表格/图片与撤销修复；六个前批新增单测仍在门禁中。
3. **中文源码路径打包审计。** 原 ASCII 解码无法识别 UTF-8 中文路径。现在检查 UTF-8、UTF-16 和 Windows 路径大小写差异；新产物已用更新脚本重新完成打包验证。

版本源 `cmake/NeoEditorVersion.cmake`、打包脚本、手动 workflow 默认值统一为 0.1.1。中英文根 README 的开发命令和应用发布说明已更新；0.1.0 下载链接及历史内存测量保持其真实版本身份。新标签建议 `neoeditor-v0.1.1`，匹配当前活动 workflow，不移动旧标签。

## 候选身份和构建证据

- EXE：`out/release-0.1.1-20261008/package-verified/EUI-Edits-0.1.1-windows-x64.exe`
- SHA256：`099d50f209efbd98a7284f1ba9b5bad7ab0a81a4d23e6f2065624a2ec0502b3b`
- 大小：3,709,952 字节；PE ProductName=`EUI-Edits`，ProductVersion/FileVersion=`0.1.1`，x64。
- 全新检查目录：`build-release-check-011-20261008`；正式脚本 **51/51**，88.18 秒测试墙钟。日志 `out/release-0.1.1-20261008/fresh-check.log`；不是全量 91 单测或全量性能验收。
- 全新打包目录：`build-release-package-011-20261008`；静态框架/CRT、bundled 依赖、关闭夹具和 CURL。日志 `package.log`；更新路径审计后 `-SkipBuild` 复核日志 `package-verify.log`，同一 EXE 哈希。
- 打包脚本通过版本、x64、系统 DLL 导入、开发路径检查、空目录内嵌许可证导出和旁置 `.sha256` 校验；PowerShell Parser、i18n、CRLF-aware whitespace 检查通过。
- 工作树文件清单与 SHA256：[机器清单](measurements/release-candidate-0.1.1-2026-10-08.json)。该清单排除自身哈希，不包含忽略目录的全部构建产物；候选和关键证据另行列入。

## 新包实机验收

Windows 11 10.0.26300、Intel Core Ultra 9 275HX、约 32 GiB RAM；本轮明确设 `NEO_D2D_SOFTWARE=1`，浅色主题，独立 APPDATA。通过 Computer Use 操作候选路径返回的唯一窗口；另以进程映像路径确认 PID 所属。测试夹具、配置、截图和退出记录保留在 `out/release-0.1.1-20261008/`，没有操作用户文稿。

| 检查 | 结果及证据 |
| --- | --- |
| Markdown、中文/emoji、表格初始显示 | PASS，`gui-acceptance/01-markdown-open.png` |
| 表格数值单元格落点 | PASS，苹果的 `2` 编辑成 `29`，香蕉的 `3` 保持；保存后读取文件核对，`02-table-edit-saved.png` |
| 表格撤销/保存 | PASS，Ctrl+Z 后保存，文件恢复苹果 `2`、香蕉 `3` |
| 图片文件导入、即时显示 | PASS，原生选择器导入合成 PNG，保存后核对绝对路径 Markdown 链接，`03-image-inserted.png` |
| 图片单步撤销 | PASS，一次 Ctrl+Z 去掉图片、恢复原段落；保存后核对文件，`04-image-undone.png` |
| 首代目录结果和当前文件定位 | PASS，153 个根条目扫描后可用，当前文件定位至目录末端；`05-library-first-root.png` |
| 基础滚轮正/反方向 | PASS，正文下移后返回顶部，`06-wheel-down.png`；不等于 OPT-001 高频压力复现 |
| 普通文本中文/emoji 输入与保存 | PASS，追加 ` 验收🙂`，保存后 UTF-8 文件内容匹配，`07-plain-saved.png` |
| 打开另一根、切回暖根 | PASS，第二根只显示自己的文件/正文，旧根目录与文稿正常恢复；`08-new-root.png`、`09-warm-root-return.png` |
| 带文件参数冷启动 | PASS，另一份全新配置，启动直接打开 Markdown 并采纳首代目录；`startup-acceptance/01-cold-cli-startup.png` |
| 正常退出 | PASS，两个自有进程 PID 9780 / 20528 均退出 0，分别见两个 acceptance 目录的 `exit.json` |

部分 native picker 的 accessibility 焦点/值设置接口不可靠，本轮据截图中实际文件名焦点完成输入；失败调用后重新观察。截图保存用于验收证据。冷启动截图是在扫描采纳后获取，没有量化首次可见/可输入时间。

SKIP：真实 IME 候选交互、低配机器、硬件 GPU、多显示器/DPI 切换、Windows 10、长时拖选及原始高频滚轮轨迹。图片剪贴板粘贴在本轮通过单测，实机覆盖为文件选择器导入；不把单测等同于剪贴板全矩阵实机验证。

## 文件整合和发布前最后步骤

建议源码提交范围是机器清单中明确列出的生产/构建脚本、测试、用户文档与前序性能证据文档；研究参考材料可同批收录为文档，但不据此增加产品能力。忽略的 `out/` 和构建目录保留在本机，不进入源码提交。前序五批原始证据也保留。

提交和发布需另行明确授权，当前已有可审查结果。实际发布时：

1. 按机器清单逐项选择文件，提交整合内容；如修改生产代码，重新构建并验收新哈希。
2. 将发布文案中的“候选、尚未发布”更新为发布时的真实状态；根 README 下载链接切到真正创建的 `neoeditor-v0.1.1` 发布页，历史 0.1.0 内存数据不改版本。
3. 核对远端分支/同名 tag 和 GitHub 凭据，按授权推送并创建 `neoeditor-v0.1.1`；当前 workflow 标签流程只创建草稿。随后检查远端构建及草稿附件哈希，再按授权公开。

本轮只读查询：远端 tags 仅有 `v0.1.0`；GitHub public Release API 仅有已发布 `EUI-Edits 0.1.0`（2026-10-04）。没有 fetch 更新 tracking ref。`gh` 本机未登录，公开只读查询使用 GitHub API；没有尝试登录或远端写入。

## 暂缓的进一步改进

显式新建/删除/改名/手选目录仍可同步刷新；本轮没有全面异步化，因为调用方依赖即时列表反馈。缓存发布后的自动淘汰需设计明确活动根保护；本轮保留已有采纳后 prune 行为。目录 rows 展开、排序及旧快照最后引用的释放仍可能在主线程；不能把后台准备表述成采纳过程完全无成本。OPT-001/OPT-002、40 案例未覆盖项继续沿用原记录，不代填 PASS。
