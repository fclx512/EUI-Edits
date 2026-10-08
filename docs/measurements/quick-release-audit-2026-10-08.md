# 快速发版改进盘点（2026-10-08）

只读审阅记录。工作区 `D:\编译开发\EUI-Edits`，审阅时 `main` / `44123ad8412010f01632763aa99824e7545bbbec`；已有五批性能补丁及相关文档处于未提交状态。本记录不代表接受这些改动或验证发布候选。

## 可快速落地的改进

1. **同步缓存目录扫描结果并暴露部分扫描告警。** `apps/neo_editor/state/app_actions.cpp` 的 `refreshVault()` 在用户新建、重命名、删除、打开文件、换根时调用 `vaultcache::requestScanSync()`，主线程扫描完成后立即更新列表；旁边已有异步 `requestVaultRefresh()` 及 `adoptVaultScan()` 流程。触发条件是任一显式操作落到 `refreshVault()`，大目录可能卡住 UI。低风险方向是仅对已知可直接更新的操作做增量快照更新，或将刷新改为后台扫描并保留旧列表/加载反馈；不要把所有文件系统变动一律异步化，需确认调用方对“操作后立即出现”的依赖。验证应覆盖新建/改名/删除/打开/换根、监听变更、扫描失败及并发重扫，并测大目录主线程阻塞时间。

2. **让目录缓存预算在扫描完成后及时生效。** `apps/neo_editor/state/vault_cache.cpp` 的 `publishLocked()` 仅发布快照并计估算字节；淘汰只由 `app_actions.cpp` 的 `adoptVaultScan()` / `refreshVault()` 后续调用 `prune()`。若多个根的后台扫描完成而当前根未采纳（例如后台页/快速换根），缓存计数和估算字节可暂时超过 3 根/32 MiB 初始预算。可在发布后安排一次有明确活动根语义的淘汰，或者证明并记录现有调用时序保证；不能盲目淘汰正在显示/扫描的根。验证需用测试注入扫描器并发完成多根扫描，检查活动根保护、扫描中根保护、LRU 次序和 count/byte 两种预算。

3. **发布文案按本次版本输出，避免把旧版状态带进新草稿。** `.github/workflows/neoeditor-release.yml` 对 `apps/neo_editor/RELEASE-NOTES.md` 做版本占位符替换，但原文件仍写“0.1.0 已发布”，产品段落也只指向 v0.1.0。已按确定的 0.1.1 候选更新 `apps/neo_editor/RELEASE-NOTES.md` 与 `apps/neo_editor/README.md`：明确未发布，列出修复、性能数据边界与未覆盖项；正式包及 SHA256 保留为发布后信息。

4. **标签策略已由主代理确定。** 新版本使用活动 workflow 的 `neoeditor-v0.1.1`；不修改 tag 触发规则。主代理提供的远端 API 核对显示目前只有公开 `v0.1.0`。本次 README 不新增尚未存在的 0.1.1 发布链接。

## 发版脚本及文档一致性

- 初次盘点时产品版本源、打包脚本和手动 workflow 默认值均为 `0.1.0`；主代理已将三处统一为用户确定的 `0.1.1`。根 CMake `project(EUI-NEO VERSION 0.6.0)` 是独立框架版本。
- 手动 workflow 运行打包并上传 artifact，不创建 Release；带 `neoeditor-v*` 标签才运行 `gh release create --draft`。打包脚本要求新构建目录和空输出目录，检查静态运行时、版本、x64、DLL 导入、源码路径和空目录许可导出。
- `apps/neo_editor/RELEASE-NOTES.md`、`apps/neo_editor/README.md` 与 `docs/NeoEditor-发布构建说明-2026-10-02.md` 是历史/现行信息混合：历史构建说明顶部明确其内容为历史记录；README 指向现行脚本。发布 notes 被 workflow 当作新草稿模板，已按本轮候选口径更新。
- 当前远端 tag 查询只证明查询时上述 tag 名称的远端可见性，未查询 GitHub Release API，也未 fetch；不能据此判断远端分支是否最新或发布页面状态。

## 已提交 `44123ad` 范围

提交主题为“修正表格光标映射与 Markdown 图片插入”。变更涉及输入模型里的单元格独立光标映射、表格/任务框行为；图片剪贴板读取、粘贴为附件、PNG/位图与本地图片文件路径导入、绝对路径 Markdown 链接；插入后预览、焦点回到编辑器、单步撤销；相应 i18n、测量记录和定向单测。提交记录写有 Win32/Direct2D 定向测试 17/17 和实机验收。发版说明需将表格及图片修复与未提交的五批优化一起审阅，但不要把性能测量移植到不同哈希候选上。

## 本轮边界

本盘点记录的源码和候选范围未因本轮文档更新而变化。文档编辑仅涉及本盘点、`apps/neo_editor/RELEASE-NOTES.md` 与 `apps/neo_editor/README.md`；未构建、未运行 GUI、未执行提交、推送或标签操作。
