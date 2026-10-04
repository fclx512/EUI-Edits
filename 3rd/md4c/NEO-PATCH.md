# 3rd/md4c 本仓库补丁说明（NEO-PATCH）

本文件记录 EUI-NEO 对 `3rd/md4c/src/md4c.c`、`3rd/md4c/src/md4c.h` 打过的**全部**功能性补丁，
供后续升级 md4c、以及审阅"第三方目录是否被动过"时对照。

上游基线：`4d3d88b feat: add markdown component`（md4c release-0.5.3，构建回落下载地址见
`3rd/dependencies.cmake` 的 `release-0.5.3.zip`）。补丁共两个提交，**分工不同，不可笼统合称**：

| 提交 | 日期 | 改动量 | 一句话定位 |
| --- | --- | --- | --- |
| `f4bcd0f` | 2026-09-24 | `md4c.c` **+39**、`md4c.h` **+42** | **源码偏移补丁（S0）**：公开两个可选回调 + `MD_CTX` 游标字段 + span/block 源区间上报，是通用机制 |
| `91a44cb` | 2026-09-25 | `md4c.c` **+13** | **表格单元格 `block_source` 补口（S3f-0）**：只补"表格 cell 到不了通用上报点"这一个缺口 |

`f4bcd0f` **不是**表格补丁（表格只是它覆盖的众多块类型之一）；`91a44cb` **才是**专门针对表格 cell
的追加补口。两者合计 = `md4c.c` 52 行、`md4c.h` 42 行，全部为新增，零删除。

---

## 1. `f4bcd0f` —— 源码偏移补丁（`md4c.c` +39 / `md4c.h` +42）

提交标题：`feat(live-preview): Live Preview S0-S3 全量落地 + 字体/渲染修复（S3d 斜体收尾）`，
其中 md4c 部分即 S0"md4c 源码偏移补丁（纯新增）"。

### 1.1 `md4c.h` 新增 42 行 —— 公开的部分

- **新增类型 `MD_SOURCE_LINE`（5 行）**：`{ MD_OFFSET beg; MD_OFFSET end; }`，作为 `block_source`
  行数组的公开元素类型；它与 `MD_LINE{beg,end}`、`MD_VERBATIMLINE{beg,end,indent}` 前两个成员同构，
  因此消费者可以按 stride 直接重解释读取。
- **`MD_PARSER` 新增两个函数指针字段（37 行，含大段契约注释）**：
  - `void (*block_source)(MD_BLOCKTYPE type, MD_OFFSET beg, MD_OFFSET end, int n_lines,
    const MD_SOURCE_LINE* lines, int verbatim_lines, void* userdata);`
  - `void (*span_source)(MD_SPANTYPE type, MD_OFFSET beg, MD_OFFSET end, int enter, void* userdata);`
  - 两个字段都**可为 NULL**（可选），位置插在 `text` 与 `debug_log` 之间；后续 `debug_log`、`syntax`
    两个原有字段的偏移因此后移（见 §4 风险 1）。
  - 头文件注释明确承诺：二者是 **additive**，设与不设，md4c 其余输出**逐字节一致**。

回调语义（照抄自头文件注释的要点）：

- **`block_source`**：每个叶子块调用一次（DOC/容器块不报，它们没有行数组）。`[beg,end)` 是**内容**
  区间——外层容器标记（blockquote 的 `>`、列表 `-`、任务 `[ ]`、缩进）已被剥掉，`beg` 正是隐藏标记时
  需要停下的位置；不含 `'\n'`。`lines` 给出该块每一源行的内容区间，**stride 因块类型而异**：
  `MD_BLOCK_CODE` / `MD_BLOCK_HTML` 用 `MD_VERBATIMLINE`（beg,end,indent，每行 3 个 `MD_OFFSET`，
  内容合并成一条），其余类型用 `MD_LINE`（每行 2 个 `MD_OFFSET`）；用 `verbatim_lines` 区分。
  数组由 md4c 持有，**只在回调内有效**。
- **`span_source`**：每个行内 span 的 opener/closer 各报一次标记本身的字节区间，`enter != 0` 表示
  opener（`**粗体**` 一次 run 报两次：开 `**` 与闭 `**`）；链接的 closer 覆盖 `](destination)` 整段尾部，
  正是 live preview 要隐藏的部分。**宽松自动链接（裸 URL）的 mark 是 URL 自身的单字符，不可隐藏。**

### 1.2 `md4c.c` 新增 39 行 —— 实现的五处挂点

| # | 位置 | 行数 | 内容 |
| --- | --- | --- | --- |
| 1 | `struct MD_CTX_tag`（约 L168） | 6 | 新增游标字段 `OFF cur_mark_beg;` `OFF cur_mark_end;`：记录"正在处理的 mark"的字节范围 |
| 2 | `MD_ENTER_SPAN` 宏（约 L462） | 4 | 宏开头：`span_source != NULL` 时先报 `span_source(type, cur_mark_beg, cur_mark_end, 1, ud)`，再走原 `enter_span` |
| 3 | `MD_LEAVE_SPAN` 宏（约 L475） | 4 | 同上，`enter` 传 0，再走原 `leave_span` |
| 4 | `md_process_inlines`（约 L4244） | 3 | 命中一个 mark、进入 `switch(mark->ch)` 之前写入 `cur_mark_beg/cur_mark_end` |
| 5 | `md_process_leaf_block` 尾部（约 L4856） | 22 | 通用 `block_source` 上报点：按 `MD_BLOCK_CODE/MD_BLOCK_HTML` 走 `MD_VERBATIMLINE`（`verbatim_lines=TRUE`），其余走 `MD_LINE`（`FALSE`）；区间取首行 `beg` 到末行 `end` |

合计 6+4+4+3+22 = **39 行**。上报点位于 switch 之后、`MD_ENTER_BLOCK` **之前**，因此事件顺序固定为
`block_source → enter_block`（消费者依赖此顺序，见 §4 风险 3）。

用 `MD_CTX` 游标 + 宏内一次性上报，而不是逐个 span case 手工插桩：这样不必改动 `md_process_inlines`
里几十个 `case`，也就不会碰到任何原有控制流。

### 1.3 本仓库内的消费者

- `apps/neo_editor/model/lp_plan.cpp`：`parseWithMd4c()` 设置 `parser.block_source = mdOnBlockSource`、
  `parser.span_source = mdOnSpanSource`（Pass B，与自带行扫描器 Pass A 相互印证，分歧累加
  `stats.markerMismatch`）。`MD_PARSER parser; std::memset(&parser, 0, sizeof(parser));` 整体清零，
  新字段天然为 NULL/赋值，无初始化遗漏风险。
- `components/markdown.h` 的 `parseMarkdownBlocks()`：`MD_PARSER parser = {}`，**不设**这两个回调 →
  走的是与上游完全一致的原生事件流，本次补丁对它零影响。
- 注意 align（`:-:` 列对齐）的信息通路**不经过补丁**：cell 的 `block_source` 只给区间，`align` 由紧随
  其后的 `enter_block(TH/TD, &det)` 带出，由 lp_plan 侧配对补齐。

---

## 2. `91a44cb` —— 表格单元格 `block_source`（`md4c.c` +13）

提交标题：`feat(md4c): S3f-0——表格 cell 区间补入 block_source（第二次补丁，纯新增）`。

**为什么还要第二个补丁**：`md_process_table_row` 按管道偏移逐格调用 `md_process_table_cell`，
但 cell **不进块树**，因此到不了 §1.2 第 5 处那个通用 `block_source` 上报点（它挂在
`md_process_leaf_block`，只对叶子块走）。结果是 live preview 定位不到"某行第 3 格"的源码区间。

**怎么补的**：在 `md_process_table_cell` 里、`line.beg = beg; line.end = end;`（trim）之后，
补一次（13 行 = 1 个 `if` + 8 行注释 + 2 行调用 + 1 个右花括号 + 1 个空行）：

```c
if(ctx->parser.block_source != NULL) {
    ctx->parser.block_source(cell_type, beg, end, 1, (const MD_SOURCE_LINE*) &line,
                             FALSE, ctx->userdata);
}
```

紧跟其后的仍是原来的 `MD_ENTER_BLOCK(cell_type, &det)` → `md_process_normal_block_contents` →
`MD_LEAVE_BLOCK`，**控制流一行未动**：enter/leave/text 事件流与未打补丁快照逐字节一致
（提交信息：5 篇样例 10037 行，含新增表格样例；`lp_plan_test` 6 篇 2744 区间 0 违规）。

实证记录在 `参考/tools/lp_probe/block-source-table.txt`（该目录在 `.gitignore` 的 `/参考` 规则下）：
TABLE → TH×3 → TD×3 → TD×3 → …；反引号内 `` `x|y` `` 未被拆格、转义 `\|` 保留、空首格为 `[189,189)`
空区间、分隔行不产生 cell。

---

## 3. 设计目的

Live Preview 要把**解析结果映射回 `md_parse` 输入的原始字节**：隐藏 `# `、`**`、`|`、`[x]` 等标记，
给活动块、代码块底色、引用竖条定位区间。md4c 原生只给 `enter_block/leave_span/text` 事件：`text`
只带内容片段的指针与长度，`enter_span/leave_span` 只带类型与 detail——**没有任何一处给出"标记字符"
的源区间**（偏移得靠指针相减去猜，且标记本身不进 text），也就无从可靠隐藏。

补丁因此提供**只增不改**的两条旁路：

1. `block_source` —— 叶子块（含表格 cell）的内容区间与逐行区间；
2. `span_source` —— 行内标记字符本身的区间。

约束条件决定了实现形态：

- **可空**：不设置回调就完全走原路，`components/markdown.h` 这类老调用方零感知；
- **不改控制流**：不新增/不跳过任何 `enter/leave/text` 事件，保证"打补丁的 md4c"与"未打补丁的 md4c"
  对同一输入产生逐字节相同的事件流，使回归验证可以退化成一次 A/B 文本比对（`参考/tools/lp_probe/`
  里 `lp_probe`（带补丁）与 `lp_probe_orig`（`orig/` 即 `4d3d88b` 基线副本）成对构建）；
- **零删除**：所有改动都是 `+` 行，便于用 `--numstat` 机械复核（§5）。

---

## 4. 兼容 / 升级风险

1. **`MD_PARSER` 结构体变化 + 中段插入（最重要）**：新增的两个字段插在 `text` 与 `debug_log` 之间，
   既让结构体变大，又把 `debug_log`、`syntax` 的偏移整体后移；而 `md_parse()` 开头是
   `memcpy(&ctx.parser, parser, sizeof(MD_PARSER))`。**所有用到 `MD_PARSER` 的编译单元必须与
   `md4c.c` 用同一份头文件全量重编**；旧目标文件 / 旧的已安装 `md4c.h` 与新 `md4c.c` 混链，会按错误
   偏移读到越界内存里的 `debug_log`（解析出错时才调用 → 表现为"偶发崩溃"）。
   且 `abi_version` 仍是 0、`md_parse` 只判 `!= 0`，**这次结构体变化没有被 abi_version 挡住**，
   错配是静默的，只能靠"全量重编"兜底。
2. **安装面**：`CMakeLists.txt` 会把 `3rd/md4c/src/md4c.h` 安装到
   `${CMAKE_INSTALL_INCLUDEDIR}/eui-neo/3rd/md4c/src`，装出去的头文件带补丁字段；下游若拿它去配
   系统自带的 md4c 库，同样落入风险 1 的错配。
3. **事件顺序契约（消费者隐式依赖）**：块的 `block_source` 必须发生在 `MD_ENTER_BLOCK` 之前；表格 cell
   必须是 `block_source(TH/TD)` → `enter_block(TH/TD, &det)` 一一配对（lp_plan 的 `pendingCellBlock`）。
   上游若调整顺序，**不会崩溃**，但 cell 的 `align` 会静默退化为 0（列对齐渲染悄悄失效）。
4. **行数组 stride 契约**：verbatim 块每行 3 个 `MD_OFFSET`、其余 2 个；读错 stride 得到的是**静默的
   垃圾区间**（不越界、不报错），所以 `verbatim_lines` 必须与 `MD_BLOCK_CODE/HTML` 的判定保持同步。
5. **宽松自动链接的 mark 不可隐藏**：`span_source` 对裸 URL 报的是 URL 自身单字符 mark，消费者需按
   空区间/约定丢弃（`lp_plan.cpp` 的 `pairSpans` 已丢弃空区间）。
6. **`MD_CTX` 多两个 `OFF` 字段**：只在 `md4c.c` 内部，随 `eui_md4c` 静态库编译，**无对外 ABI 影响**。
7. **上游不认识这些字段**：升级/换版本必须重打补丁，冲突点固定是 §1.2 的 5 处挂点与 §2 的
   `md_process_table_cell`；上游若自行重构这些函数，补丁会直接失配。
8. **"事件流逐字节一致"是样例实证，不是形式化证明**：由 5 篇样例 10037 行与 A/B 快照比对得出，
   样例之外的新构造仍可能暴露差异，升级后要重跑（§6、§7）。
9. **`EUI_ENABLE_MARKDOWN=OFF`**：不编 md4c，补丁与该路径无关。

---

## 5. 如何从提交 diff 复核"纯新增"

```bash
# ① 每个提交的统计：只应出现 insertions，deletions 恒为 0
git show --stat f4bcd0f -- 3rd/md4c
#   3rd/md4c/src/md4c.c | 39 +++++
#   3rd/md4c/src/md4c.h | 42 +++++++
#   2 files changed, 81 insertions(+)          ← 没有 "deletion"
git show --numstat f4bcd0f -- 3rd/md4c
#   39	0	3rd/md4c/src/md4c.c
#   42	0	3rd/md4c/src/md4c.h

git show --stat 91a44cb -- 3rd/md4c
#   1 file changed, 13 insertions(+)
git show --numstat 91a44cb -- 3rd/md4c
#   13	0	3rd/md4c/src/md4c.c

# ② numstat 第二列（删除行数）全部为 0 —— 这是"纯新增"的机械判据

# ③ 正文删行计数（排除 diff 头的 '--- a/...'）必须为 0
git show f4bcd0f -- 3rd/md4c | grep -c '^-[^-]'   # → 0
git show 91a44cb -- 3rd/md4c | grep -c '^-[^-]'   # → 0

# ④ 相对补丁前基线整体核对（4d3d88b → 91a44cb）
git diff 4d3d88b 91a44cb --numstat -- 3rd/md4c   # → 52 0 / 42 0
git diff 4d3d88b 91a44cb -- 3rd/md4c | grep -c '^-[^-]'   # → 0

# ⑤ 每段新增正文都带 EUI-NEO patch 标记
grep -c "EUI-NEO patch" 3rd/md4c/src/md4c.c       # → 4
grep -c "EUI-NEO patch" 3rd/md4c/src/md4c.h       # → 1

# ⑥ 库内存档的合并补丁与 git diff 逐字节一致（仅 index 行不同）
diff <(git diff 4d3d88b 91a44cb -- 3rd/md4c | grep -v '^index ') \
     <(grep -v '^index ' 参考/tools/lp_probe/md4c-source-offsets.diff)
# → 无输出（158 行）
```

①②③ 是核心：`git show --numstat` 的删除列全 0 + 正文删行 grep 计数为 0，即可断言这两个提交对
`3rd/md4c` 只加不改。④⑤⑥ 分别从"基线整体""标记可检索""存档可复算"三个角度交叉验证。

---

## 6. 升级 md4c 时的检查清单

**升级前**

- [ ] 记录当前基线与补丁范围：`git log --oneline -- 3rd/md4c`（应为 `4d3d88b` → `f4bcd0f` → `91a44cb`）。
- [ ] 导出补丁：`git diff 4d3d88b 91a44cb -- 3rd/md4c > md4c-neo.patch`
      （或直接用已存档的 `参考/tools/lp_probe/md4c-source-offsets.diff`，二者等价，见 §5⑥）。
- [ ] 抄录契约清单：`MD_SOURCE_LINE`、`block_source`/`span_source` 签名与注释语义、stride 规则
      （verbatim=3 / 普通=2）、事件顺序（`block_source` 先于 `enter_block`）、`MD_CTX` 游标字段。

**升级时（新源码落位后）**

- [ ] 先确认上游基线版本（`3rd/dependencies.cmake` 里 `release-0.5.3.zip` 是否同步更新；仓库内的
      `3rd/md4c` 是优先被 `eui_find_bundled_dependency` 命中的"单一事实来源"）。
- [ ] 应用补丁：`git apply --check md4c-neo.patch`；失败则手工重打，冲突点固定为 §1.2 的 5 处
      （`MD_CTX` 字段、两个宏、`md_process_inlines` 的 mark 游标、`md_process_leaf_block` 尾部）与
      §2 的 `md_process_table_cell`。
- [ ] 逐条复核纯新增性质：§5 的 ①②③（对**新的**提交重跑，`--numstat` 删除列仍须全 0）。
- [ ] 核对 `MD_PARSER` 布局：新增两字段仍在 `text` 与 `debug_log` 之间；上游若新增/重排字段，同步
      调整；确认 `abi_version` 语义未变（仍是 `!= 0` 即拒绝）。
- [ ] 核对 `MD_LINE` / `MD_VERBATIMLINE` / `MD_OFFSET` 的定义与布局未变（stride 契约的前提）。
- [ ] 确认 §1.2 五处挂点与 §2 挂点在新源码里仍然存在、语义未被上游重构（尤其 `md_process_inlines`
      的 mark 循环与 `md_process_leaf_block` 的尾部）。
- [ ] 全量重编（风险 1）：删掉旧目标文件/缓存，重新 configure + build，杜绝新旧头文件混编。
- [ ] grep 补丁标记：`grep -c "EUI-NEO patch" 3rd/md4c/src/md4c.c`（4）、`.../md4c.h`（1）。

**升级后对账**

- [ ] `ctest --test-dir build-tests -C Release -R lp_decorations --output-on-failure`
      （见 §7；它把 `lp_plan.cpp` 一起编进测试，直接打在补丁的消费者身上）。
- [ ] A/B 事件流比对：用 `参考/tools/lp_probe` 的 `lp_probe`（带补丁）与 `lp_probe_orig`
      （`orig/` 基线，注意该基线是 `4d3d88b` 版本，**不是** `HEAD`——CMakeLists 注释里的
      `git show HEAD:...` 已过期）对同一批样例做逐字节比对。
- [ ] 跑 `lp_plan_test`（6 篇 2744 区间 0 违规）与 `lp_probe --block-source`
      （表格样例见 `参考/tools/lp_probe/block-source-table.txt`）。
- [ ] 抽查 `stats.markerMismatch == 0`、`stats.invalidRanges == 0`（`lp_decorations` 启动即断言）。
- [ ] 回归 `components/markdown.h` 路径：不设两个回调时输出应与上游一致（事件流 A/B 比对覆盖）。
- [ ] 若 md4c.h 变化，检查安装规则（`CMakeLists.txt` 安装 `3rd/md4c/src/md4c.h`）仍指向补丁后的头文件。

---

## 7. 对账命令与相关测试

已受控的对账命令（在仓库根目录执行）：

```bash
ctest --test-dir build-tests -C Release -R lp_decorations --output-on-failure
```

本次实测：`1/1 Test #26: lp_decorations ... Passed 0.02 sec`，`100% tests passed`；直接跑
`build-tests/Release/lp_decorations.exe` 输出 `ALL PASS：227 项检查，0 项失败`（内含
`stats.usedMd4c`、`markerMismatch == 0`、`invalidRanges == 0` 断言，直接压在补丁的消费者
`lp_plan` 上）。

补充说明：

- 本机默认 PATH 上没有 `ctest`，需使用 CMake 自带的那份
  （`D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe`，
  与 `build-tests/CMakeCache.txt` 里的 `CMAKE_COMMAND` 一致）。
- `lp_decorations` 由仓库根 `CMakeLists.txt` 对 `tests/unit/*.cpp` 做 GLOB 注册，并额外把
  `apps/neo_editor/model/lp_plan.cpp`、`text_file.cpp` 编进该目标。
- **可选的独立 harness**：`参考/tools/lp_probe/lp_plan_test.cpp` 位于 `.gitignore` 第 5 行 `/参考`
  规则之下（`git check-ignore` 可验证），是**独立于 build-tests 的**自带 CMake 工程
  （`参考/tools/lp_probe/CMakeLists.txt`，不链接 eui/core，可脱离窗口跑），需要单独 configure，
  **不属于 build-tests，也不被上面的 ctest 命令覆盖**。
- 关于 CI：本文件**不声称** CI 一定会为纯 markdown（`3rd/md4c` 下的文档类）改动运行任何流水线；
  一切对账以上述本地命令的实测结果为准。

---

## 8. T20 变更范围

T20 只新增本文件 `3rd/md4c/NEO-PATCH.md`；`3rd/md4c/src/md4c.c`、`md4c.h` 及其它任何文件均未改动，
也未执行 `git add` / `git commit`。
