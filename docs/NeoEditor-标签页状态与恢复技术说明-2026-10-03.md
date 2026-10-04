# 标签页状态与恢复技术说明

日期：2026-10-03。适用于本仓库 Win32 + Direct2D 的第一版多文档标签。本轮没有扩展性能专项。

**最新关闭契约**：正常关闭完成保存/丢弃确认后清理会话，下次空白启动，不自动打开旧干净标签或last_file；异常未保存草稿继续恢复，损坏恢复证据不自动删除。worker停止后的shutdown不再提交/等待会话写；由消息循环尚存时的异步finalizer串行排完旧写再清理。本文旧版恢复/退出flush描述以[关闭生命周期说明](NeoEditor-关闭生命周期与基础内存技术说明-2026-10-03.md)替代。

后续接手提示：切页流畅/内存协同优化已实施，实施结果与边界见下文"性能改造后的运行策略"一节：库扫描按根共享并后台化、派生缓存按页有界复用、干净页后台核验、会话写串行后台化 + 内容寻址 body。编辑状态、文件安全和恢复原子性契约保持不变。

## 状态所有权

`state/app_state.h` 中 `AppState` 保留主题、菜单、设置及保存事务；它继承 `DocumentSession`，让既有编辑与保存函数继续访问活动文档。后台文档移动到 `inactiveTabs`，显示顺序由 `tabOrder` 保存，身份使用稳定 `uint64_t TabId`。不能以数组位置或路径代替身份：顺序能改变，未保存页无路径，另存或重命名也会改变路径。

`DocumentSession` 保留正文、编码/BOM/换行、路径、修改代次、磁盘指纹、恢复来源、语言、换行覆盖、折叠和查找条件；其 `VaultContext` 保存根、展开项、筛选、选择和滚动。`editorMemory` 保存控件的正文/撤销/选区/光标/滚动。切页使用移动，不复制正文与撤销栈。活动输入控件仍在 `Ui::StateStore` 中，`syncDocumentTabInputs` 在下一次 compose 时交接，须在 `editorStateDirty` 重置和正文构建之前调用。

后台页保留编辑状态、库视图投影与**预算内的派生缓存**（LP 计划/装饰表/源码高亮 + 输入排版），不再每次切页清空；超预算时按 LRU 逐出并 `swap` 释放容量，编辑状态（正文、撤销栈、光标/选区/滚动）不受影响。全局单例缓存（`lp::planCache`/`lp::decorationCache`/`source::cache`）当前承载**活动页**：切页时在 `captureDerivedCaches`/`installDerivedCaches` 里整套搬进/搬出每页槽位，因此不可能出现"A 的计划配 B 的装饰表"的跨页误命中。第 3 节的"切页清缓存、每次切页持久化"是本文件早前版本的行为描述，已不再适用。

## 性能改造后的运行策略（2026-10-03）

- 库扫描（阶段 B）：`state/vault_cache.*` 按规范化根共享只读 `ScanResult`；`VaultContext::vaultScan` 改为 `shared_ptr<const vault::ScanResult>`，每页只保存 `rows` 视图投影与 `vaultRowsGeneration`。切页走 `adoptVaultScan`（命中即复用、否则排后台扫描），显式文件操作仍走 `refreshVault` 同步一次以保证列表即时正确；目录事件防抖后 `requestVaultRefresh` 后台重扫。预算默认 32MiB/3 根。
- 派生缓存（阶段 C）：预算默认 64MiB / 最多 2 个非活动页，`NEO_TAB_CACHE_BUDGET_MIB`、`NEO_TAB_CACHE_PAGES` 可覆盖。字节记账深入行内向量（`spans/conceal/cells`、`holes/runs`、`metrics.byteIndices/caretX`、`LineGeometryTable`）。
- 文件核验（阶段 D）：干净页激活发后台核验，指纹未变不解码；结果需页面/路径/正文代次/干净状态全部吻合才应用，期间编辑或换路径一律丢弃。
- 会话写（阶段 E）：`state/session_writer.*` 单一串行 writer，队列上限 1 in-flight + 1 最新待提交；提交是**拥有的不可变快照**，dirty正文按 `(TabId, revision)` 复用，clean页立即释放复用引用。`committedSeq` 只在manifest提交成功后推进；失败保留上一完整快照。`flushDocumentSession` 仍是显式checkpoint工具，正常退出不调用它；关闭finalizer拒收新写、等唯一旧in-flight完成后清理，保持事件循环可响应，清理失败不授权退出。
- 诊断：`NEO_TABS_TRACE=<file>` 显式开启 QPC 分段记录（默认关闭，阶段名含 `switch/cache-save/cache-restore/cache-evict/vault-scan/file-check/result-apply` 等）。

所有编辑器 ID，包括第一页，统一为 `editor.input.<TabId>`。运行时先应用新焦点请求，再处理已释放 scope，并对 `scope + "."` 前缀清焦点。若第一页仍使用 `editor.input`，释放它就会误伤 `editor.input.2.hit`，导致新页第一次输入失效。实机验证发现并修复了这个问题；其他组件采用分层 ID 时应避免同级实例成为彼此的 scope 前缀。

标签列表和悬停提示使用 `TextPrimitive::measureTextSize` 测量真实换行，传入与渲染相同的字体、宽度和显式行高；不要以 `ceil(总文字宽度 / 可用宽度)` 估计高度，路径的断词规则会多出一行并挤到下一条标题。列表内已有完整详情，禁止另画悬停提示遮住行项。自定义滚动回调收到 Win32 原始滚轮方向，纵向偏移要减去 `event.y`，横向则加 `event.x`；运行时不会替自定义回调翻转方向。实机覆盖列表滚到顶部并点击第一条。

## 库根与文件身份

`sameDocumentFile` 优先使用 `filesystem::equivalent` 识别硬链接等物理别名，失败再按规范路径组件比较；Windows 使用 `CompareStringOrdinal` 忽略大小写。`documentWithinRoot` 按组件检查包含，避免 `project` 错配 `project-extra`。外部打开按当前根是否包含文件决定继承或使用父目录，从库树打开继承库根；另存到库外跟随新父目录。

`mergeRelatedVaultRoots` 只从已有非空库根中选择最短祖先。扩大根时重新换算展开项、选中项及待定位路径，保留筛选/滚动；无关库不合并。每个标签仍保存独立库视图状态。手动选目录后也应用此规则；关闭某页不缩小已经合并的根。

`vault_view.h` 每帧收集属于当前根的打开文档，给文件及包含这些文件的目录画圆点。活动行另有选中条。标签细色条只辅助辨认，根与文件路径才决定身份、附件目录和文件操作目标。第一版仅监听活动库，切根清旧监听的防抖状态。

## 保存与关闭事务

重复打开激活已有页，编辑中的其他页继续保留。背景脏页激活时保留原始磁盘指纹，不能给未保存正文重新采集基线；背景干净页仅在验证加载成功后更新正文及指纹。另存为不能覆盖另一已打开物理文件。既有原子保存、冲突提示和保存前再次检查继续执行。

确认捕获 `confirmationTabId`。保存框、冲突、重命名、图片预览等模态期间冻结切页；内部退出流程使用专用激活入口。关闭单页只操作该页，最后一页关闭后创建空白页。退出按顺序逐个询问脏页，任一取消立即停止，之前选择丢弃的页面仍在内存中并恢复恢复资格。保存失败、保存框取消或冲突均保留目标文稿。

库重命名更新所有命中文档的路径/库根/恢复来源，保持脏页原指纹；部分删除仅把实际已不存在的文稿保留为无路径草稿，不抹去其他后台文稿。

## 原子恢复清单

`model/session_storage.*` 在用户配置目录 `session/` 中写 `manifest.json`。**v2（当前写入格式）**：清单含 version、active ID 与每页 `id/path/vaultRoot/language/wrapOverride/dirty`；脏页另存编码、代码页、BOM、换行、`bytes` 与 `body` 文件名，正文按**内容寻址** `body-<contentid>.utf8` 写（未变正文零字节写入、可跨页去重）。**v1**（`generation` + `body-<generation>-<id>.utf8`）保持**只读兼容**，下一次写入即迁移为 v2，迁移成功后由 cleanup 清掉旧 generation 文件。空脏文稿也有合法的空 body。

先原子写完新 generation 的所有 body，再原子提交清单，提交成功后才删除不再被引用的 body。失败保留原清单及其 body。切页只改 active/order 元数据（不再重写未变正文），输入恢复写仍按既有间隔节流并增加"自动到期"补写。只有完整清单提交成功后才清旧 `recovery.txt`，旧单页恢复可迁移。

启动时每份脏稿都恢复为独立未保存文稿，原路径只作来源，不自动覆盖原文件；命令行请求可与这些草稿并存。干净文件暂时无法加载时保留路径及错误占位页，允许重试，不静默删页。恢复清单里重复的干净物理文件合并为一页；脏记录始终各自保留。

读取严格校验 UTF-8、ID、字段、generation 和 body；限制为 256 页、单 body 256 MiB、总 body 1 GiB。缺清单等于无会话；损坏清单或缺 body 返回失败且不清理证据。应用随后阻止自动重写，并在编辑时与状态栏提示恢复不可用，手动保存仍可用。恢复写失败也有持续状态提示。该保护不会自动修复损坏清单，需保留原配置另行排查。

## 回归入口

- `tests/unit/document_tabs.cpp`：父子根/组件边界、去重/硬链接、每页撤销/选区/滚动/折叠、背景冲突、关闭/退出取消、另存冲突、背景重命名/部分删除、不可用页重试与恢复去重。
- `tests/unit/session_storage.cpp`：顺序/元数据、Unicode/空草稿、写入故障、非法 ID/重复 ID/**v2 body 路径穿越**、**v1 只读兼容**、旧恢复文件保留。
- `tests/unit/vault_scan_scheduler.cpp`：同根扫描去重与重扫标记、完成时根失效丢弃、活动根不被修剪。
- `tests/unit/session_write_scheduler.cpp`：串行写序、待提交取代、正文复用、失败不推进 committedSeq、forgetTab。
- `tests/unit/file_check_scheduler.cpp`：未变不解码、同尺寸同时间戳变化可发现、缺失、restart 取代。
- `tests/unit/document_tab_cache.cpp`：暖切不重建计划、计划归属当前页、预算 LRU 逐出并释放排版容量。
- `tests/probes/document_tabs.py`：隔离 APPDATA/TEMP、拥有窗口与前台校验、菜单同行卡片/列表、两份真实恢复稿独立保存、外部冲突、关闭取消、正常退出与只读内存取样（已按 v2 body 与后台提交改为等待 manifest 状态）。
- `tests/probes/document_tab_performance.py`：切页可见时延（小区域截图轮询）+ 同 PID 内存，支持 `--library-files/--body-kib/--rounds`。
- `tests/probes/document_tab_stability.py`：同 PID 长时固定负载的 private commit 采样与分窗中位数。

探针通过私有 TEMP 请求验证延迟打开的消费；没有验证第二进程通过全局单实例事件转发。UI 缩放不等同于真实多屏 DPI 切换。实机 WM_CHAR 与模型合成态清理也不能替代中文输入法候选态切页验收。
