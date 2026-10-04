# NeoEditor 关闭生命周期与基础内存

日期：2026-10-03。针对 Win32 + Direct2D、轻量记事本与常用 Markdown。用户要求修复关闭约数秒无响应、正常关闭不恢复旧文档，并调查进一步节省基础内存的方法。

## 关闭为什么固定卡约 5 秒

`include/eui/detail/dsl_app_impl.h` 的应用退出先停止并回收 `core::async`，再调用用户 `onShutdown`。旧 `app.cpp` 在 `onShutdown` 中又调用 `flushDocumentSession(state, 5000)`。writer 在 worker 停止之后无法提交，原实现却仍标记 in-flight，commit ack 永远不会到达，只能等满 5000ms。此前关闭请求中已经执行过一次会话写入；这个收尾写既重复又违反异步生命周期。

修复保留框架退出次序，从应用 shutdown 中移除会话写与等待；writer 检查 `async::restart` 的返回值，提交被拒必须立即结算失败。settings、字体安全标记、诊断日志与单实例服务的收尾仍执行。

## 正常关闭的事务

```text
WM_CLOSE → 按标签顺序询问未保存文稿
         → 任一取消/保存失败/未解决冲突：保留窗口与草稿
         → 全部处理完：sessionClosePending=true
         → writer 拒收新提交，丢弃尚未开始的 pending，释放复用正文缓存
         → 唯一已开始的旧写入完成
         → 后台清理会话清单、自有 body、legacy recovery.txt
         → 主线程收到成功回调 → closeApproved → requestClose
         → worker shutdown → settings/font/trace/instance 收尾
```

清理期间 WM_CLOSE 立即返回 veto，消息循环继续运行；compose 使用不可编辑的关闭提示，键盘、正文回写、拖入与单实例外部打开被拦截。重复 WM_CLOSE 不重启事务。关闭请求由 finalizer 唯一发出，`finishPending` 不再重复发送。

writer 仍只有 1 个 in-flight 和 1 个最新 pending。清理必须排在已经运行的写之后，否则旧 worker 可能在删除后重新写出清单。写入与删除同时使用存储写锁。清理不递归删除配置目录，只处理受管理文件；其他文件保留。删除错误返回失败，恢复接受提交、还原 suppressRecovery 并尝试重新持久化，窗口显示错误且可以重试，不把清理失败当退出成功。

| 情况 | 行为 |
| --- | --- |
| 正常保存/丢弃后关闭 | 删除打开标签记录、恢复正文与 legacy 恢复文件；清空自动启动 last_file/vault；下一次空白启动 |
| 无命令行启动，存在旧干净清单/last_file | 忽略，不自动打开文件或干净空标签 |
| 明确通过命令行/打开操作指定文件 | 打开指定文件；失败时提示，不转而打开无关的旧 last_file |
| 异常退出留下完整未保存草稿 | 保留恢复能力；不恢复其他未被指定的干净文件 |
| 用户取消关闭、保存失败或取消另存 | 仍保留正文、撤销、标签与恢复资格 |
| 恢复清单损坏，sessionStorageBlocked | 保持既有恢复错误提示，正常关闭跳过恢复文件清理，保留无法验证的文稿证据；清空 last_file/vault。此例外不会自动打开旧干净工作区 |

用户设置与手动“最近文件”入口继续保留；最近文件不触发自动重开。损坏存储不自动覆写或清理是已有防丢稿契约：正常关闭规则不授权静默丢弃无法读取的唯一正文。

## 已保存页为什么仍可能占一份恢复正文

writer 为脏页生成 `shared_ptr<const Document>`，按 `(TabId, revision)` 复用，避免后台读活动文本。此前页保存变干净后，`lastDocs` 和 revision 表仍持有这份完整正文，直到关闭页或重置 writer。

现在 clean record 立即移除对应复用条目，dirty 新 revision 替换旧条目，forget/reset/正常关闭同步释放。已开始或排队事务仍持有自己的快照直到结算，不能提前修改或释放。`Stats::retainedBodyBytes` 估算复用表的 `sizeof(Document)+text.capacity()`，**不包括** in-flight/pending、活动正文、输入正文、撤销、派生缓存和分配器留存。它归零证明这张表不再持有正文，不能等同于操作系统 Private Bytes 必须立即下降。

启用 `NEO_TABS_TRACE` 后，`recovery-cache` 记录这一计数；诊断按批缓冲，应在正常退出、flush 后读取。回归验证 dirty→clean 的真实相邻计数，而不是寻找日志中任意一次零值。

## 灰度字形缓存按需增长

灰度图集此前一开始就分配 `2048×2048×1`，CPU 像素为 4 MiB，渲染后后端还会建立对应位图/纹理。现在从 `512×512`（0.25 MiB）开始，不足时扩大到 1024，再到 2048；最大容量与溢出重置策略保留。`NEO_GRAY_ATLAS_INITIAL_SIZE=2048` 是同 EXE 配对诊断开关，不是产品设置。

扩容逐行复制旧像素，旧字形 texel 坐标不变，但归一化 UV 需要乘 old/new。发布前重算共享字形索引，递增 atlas generation 与布局 epoch；旧图元发现 epoch 改变后重建 UV。一次 prepare 如果触发两次扩容，需要最多三遍才能得到同一代的所有顶点，不能混合旧页和新页 UV。后端已有 generation/dimension 检查，扩容后更新位图；OpenGL 原路径在更换图集前也会 flush 旧批次，但本批运行时验收限于 Direct2D。

真实 recording backend 测试复制 draw command、所有顶点与图集像素，验证 1500 个不同中文字形的 UV、墨迹，以及扩容前后旧文本的几何、绝对 texel 坐标和像素校验和。emoji 彩色页不跟随灰度页变化。真实 Win32 图像比较覆盖滚动到新字形、返回旧文字及改宽。

彩色图集原本已在首次 BGRA 字形时分配，仍是 1024²×4；普通无彩色字形场景不预分配。扩容后的灰度页目前不在每次切页缩小，避免反复栅格化影响流畅度。最大页途中真正溢出时丢弃混合代顶点并等待下次 prepare；一个单独图元若自身字形永久超过最大页，不保证完整显示，不能把溢出恢复测试宣传为无限字形容量。

## 后续节省内存的优先级

以下是源码候选，尚非实测收益。保留 P1 的默认 64 MiB/两个非活动页派生预算，以维持已验收的暖切体验；预算是按需填充的上限，不是启动即预分配。

| 顺序 | 候选与适用场景 | 实施与验证要点 |
| --- | --- | --- |
| 1 | `TextSizeCache` 只有 1024 项上限，key 含完整文本；长 key 可能堆积 | 先记录实际 key 字节/命中率，再加总字节预算及超大项 bypass；校验尺寸结果、淘汰、长路径 tooltip 和切页速度。另两层 metrics/shaping 已有 16/8 MiB 字节上限且按需填充，不能当作 24 MiB 基础预留 |
| 2 | 大文稿的 `DocumentSession.doc.text` 与 `InputState.text` 各持一份正文 | 先按各自 capacity 对账；考虑共享权威正文/输入引用需要重做生命周期、IME、undo、回写和恢复，不在本批贸然改。普通小稿收益很小 |
| 3 | D2D 窗口 target、保留的绘制 cache 与改宽余量 | 加物理尺寸/容量诊断，比较最大化→缩小后的长期曲线与重建次数，再评估增长余量。1120×760×4 约 3.25 MiB/原始 surface 是算术，不等于进程专用提交；当前已有 2 秒稳定且面积超两倍才缩小策略，不每次切页释放 |
| 4 | 大量粘贴、替换、删除后的 undo 长尾 | 当前保存变更 span，最多128项，普通打字不复制全文；用 `undoHistoryBytes()` 量大操作后峰值，再考虑总字节预算。淘汰会缩短用户撤销历史，需单独验收 |
| 5 | 非 ASCII 自选字体路径的文件缓冲 | 默认雅黑 ASCII 系统路径走 FT_New_Face；内置 FontAwesome 直接引用 RCDATA，无应用端整文件复制。仅少数 Unicode 自选路径走最多128MiB shared buffer；mmap/stream 涉及 Unicode IO 与 FreeType 生命周期，优先级低 |

不通过 `EmptyWorkingSet` 等 trim 人为压任务管理器数字，不禁用暖缓存、不把所有 UI 或 Direct2D 迁到 worker。空白基线、长时间不同字形、长行、大量撤销、多页与窗口尺寸应分开测量 Private Bytes、总/专用 WS 和可见时延，分别确定真正的持有者。

## 接手验证入口

- `tests/probes/close_session.py`：私有配置与 PID；退出时延/WM_NULL、空白重启、旧清单忽略、保存释放缓存、取消/丢弃/异常草稿检查。
- `tests/probes/atlas_visual.py`：同 EXE 2048 与 512 的真实客户端像素对照，中文/emoji、滚动和改宽；只排除 DWM 圆角边界。
- `tests/probes/document_tabs.py`：多脏页关闭顺序、Save/Cancel/冲突、库根与独立撤销；初次打开必须显式传文件，不依赖 last_file。
- `scripts/check-neoeditor.ps1`：新独立目录与精确测试集合检查；CTest、构建和静态测试不替代实机正常退出证据。

不要并行运行驱动前台窗口的探针；内存/性能测量时停止构建与压力测试。每次保存最终 EXE SHA、配置、采样时刻、正常退出码与原始报告。旧哈希、旧恢复行为和前批数字只能作为对应历史批次证据。
