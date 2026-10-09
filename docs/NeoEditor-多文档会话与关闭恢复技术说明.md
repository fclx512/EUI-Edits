# NeoEditor 多文档会话与关闭恢复技术说明

更新：2026-10-09。本文合并原“标签页状态与恢复”及“关闭生命周期与基础内存”两份说明，记录当前多文档状态所有权、恢复格式和正常退出事务，并说明与会话正文相关的内存边界。关键关闭、writer 与 session-storage 契约已按当前源码核对；性能数字和历史实现只在明确标注的范围内引用。

## 状态所有权与标签页

`AppState` 保存应用设置及活动文档会话；其他页由稳定 `TabId` 标识并放入 `inactiveTabs`，显示顺序单独保存在 `tabOrder`。不能用数组位置或路径当身份：顺序会变化，未保存页无路径，另存或重命名会改变路径。

`DocumentSession` 持有正文、编码/BOM/换行、路径、修改代次、磁盘指纹、恢复来源、语言、换行覆盖、折叠、查找条件和 `VaultContext`。`editorMemory` 保存输入控件的撤销、选区、光标及滚动。切页移动状态，不复制正文与撤销栈；输入控件仍在 `Ui::StateStore`，必须在正文重建前完成页间同步。实现入口见 `apps/neo_editor/state/document_tabs.*` 与 `apps/neo_editor/state/app_state.h`。

后台页可以保留有预算的派生缓存，包括 LP 计划/装饰、源码高亮和输入排版。缓存超预算时按 LRU 逐出并释放派生容量；正文和编辑状态不因此丢失。活动页使用的全局缓存随页捕获/安装，避免把一页的计划与另一页的装饰混用。详细的布局缓存键、预算与失效规则见[布局分层与切页缓存说明](NeoEditor-P1布局分层与切页缓存技术说明-2026-10-03.md)。

同一物理文件通过 `filesystem::equivalent` 优先识别，再按规范路径组件比较；库根包含关系按路径组件判断。根合并时重算展开项、选择和待定位路径，但各页保留独立的库视图状态。文件身份及标签页行为的自动化回归见 `tests/unit/document_tabs.cpp`。

## 保存、会话持久化与恢复

`session/manifest.json` 当前写入 v2，并只读兼容 v1。v2 按正文内容寻址 body 文件；写入先原子提交正文，再原子提交 manifest，成功后才清理不再被引用的受管正文。v1 在下一次成功写入时迁移到 v2。空草稿也使用合法的空正文文件。

读取会校验 UTF-8、ID、字段和正文大小；限制包括最多 256 页、单页正文 256 MiB、会话正文总计 1 GiB、manifest 16 MiB。清单损坏或缺正文时保留恢复证据、阻止自动覆写并提示用户；手动保存仍可用。脏页恢复为独立未保存文稿，原路径只作为来源，不自动覆盖磁盘文件；干净页无法加载时保留错误占位并允许重试。旧 `recovery.txt` 仍用于兼容迁移。

writer 只接受拥有的不可变快照，最多保留一个 in-flight 和一个最新 pending；正文复用按 `(TabId, revision)` 维护，页面变 clean 后释放复用引用。`committedSeq` 仅在 manifest 成功提交后推进，写入失败保留上一完整快照。`flushDocumentSession` 是显式 checkpoint 工具，正常关闭路径不调用它。

持久化和故障回归见 `tests/unit/session_storage.cpp`、`tests/unit/session_write_scheduler.cpp`；切页和页状态回归见 `tests/unit/document_tabs.cpp`、`tests/unit/document_tab_cache.cpp`。具体格式与校验以 `apps/neo_editor/model/session_storage.*` 为准。

## 正常关闭事务

退出前按标签顺序处理未保存页。用户取消、保存失败、另存取消或未解决冲突时保留窗口和草稿。全部确认后，主线程发起异步 finalizer；窗口在清理期间继续响应消息，但关闭请求被暂缓，编辑和会写入会话的操作被拦截。

finalizer 关闭 writer 的新提交入口、丢弃尚未开始的 pending、释放正文去重缓存，等待唯一已开始的写入结束，再清理会话 manifest、自有正文和兼容恢复文件。清理成功后才批准退出；失败则恢复提交/草稿恢复资格、显示错误并允许重试。`onShutdown` 发生在 worker 停止之后，只负责设置等收尾，不再等待会话写入。关键实现见 `apps/neo_editor/state/app_actions.cpp`、`apps/neo_editor/state/session_writer.cpp` 和 `apps/neo_editor/app.cpp`。

正常退出清理失败会阻止退出。另一个容易混淆的情形是普通 session 写入成功后回收旧正文：那类孤儿正文清理失败不一定使本次 manifest 写入报告失败，不能将两种“清理失败”概括成同一事务结果。

| 情况 | 行为 |
|---|---|
| 正常保存/丢弃后关闭 | 清理打开页会话与旧恢复文件，下次不自动重开旧干净标签或 `last_file` |
| 用户取消、保存失败或冲突未解决 | 保留窗口、文稿与恢复资格 |
| 异常退出留下完整脏稿 | 下次恢复为独立未保存文稿，不自动覆盖来源文件 |
| 清单损坏或正文缺失 | 保留证据并提示；正常退出不删除无法验证的恢复材料 |
| 明确从命令行或打开操作指定文件 | 打开该请求的目标；失败不回退到无关旧 `last_file` |

关闭和恢复的实机探针为 `tests/probes/close_session.py` 与 `tests/probes/document_tabs.py`。具体操作覆盖、EXE 身份及正常退出结果必须按对应报告记录，单元测试不替代真实窗口验收。

## 会话正文内存边界

writer 的正文去重表可能暂时持有脏页快照；clean、forget、reset 和正常关闭会释放相应引用。`Stats::retainedBodyBytes` 只估算复用表中的 `Document` 与文本容量，不包含 in-flight/pending、活动正文、输入正文、撤销记录、派生缓存或分配器留存。计数归零不保证进程 Private Bytes 同步下降。

灰度字形图集和其他进程基线属于独立内存主题；旧记录及候选保留在维护者本地原文中，未并入当前会话契约。

## 维护边界

标签状态、恢复和关闭属于应用契约；布局派生缓存另见专门的 P1 说明；块内几何另见[块模型契约](块模型契约-2026-09-26.md)。构建/打包命令见[发布构建说明](NeoEditor-发布构建说明-2026-10-02.md)。
