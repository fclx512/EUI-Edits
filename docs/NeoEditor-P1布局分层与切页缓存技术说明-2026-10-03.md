# NeoEditor P1 布局分层与切页缓存技术说明

本文记录 NeoEditor 大文稿 caret 度量分层、行装饰快照压缩和标签页派生缓存的源码契约，供本项目维护与其他编辑器参考。它说明实现边界和验证入口，不代表运行性能结论；实测结果应以单独的验收报告为准。

## 设计边界

编辑器仍在现有 UI 线程上构建布局。P1 将“绘制和滚动所需的逐行几何”与“命中测试、光标定位所需的逐字符 caret 停靠点”分开保存；它不把整篇文档的排版搬到后台，也不改变文档正文、换行规则或光标语义。

在 `components/input_model.h` 中，`TextLine` 保留行的起止偏移、精确宽度、字号、行高、top、软换行段、装饰、runs 和表格信息。多行布局的 `LineGeometryTable` 仍构建完整前缀几何，因此总高度、按 y 定位、折叠行高度、滚动范围和 viewport 可见区计算继续使用完整文档的几何数据。可按需省略的是普通非表格行的 `metrics.byteIndices` 和 `metrics.caretX` 数组，而不是行宽或行几何。

## Viewport caret metrics

编辑器在 `apps/neo_editor/ui/editor_view.h` 显式启用 `viewportMetrics`。压缩实际生效还要求文档 UTF-8 字节长度至少为 256 KiB（`compactMetricsEnabled`）。其他 `components::input` 调用默认关闭，除非调用方显式选择该能力。

全量布局测量时，普通非表格行可只保留精确宽度和已确定的行段几何。布局完成后，当前 viewport 可见行及前后各两行会 hydrate 完整 caret 数组；光标行及其相邻行也会 hydrate。可见范围随滚动变化，旧窗口以外且不在光标邻域的已登记行会释放 caret 数组。输入事件若查询任意文档偏移的 x、任意行的最近 caret 或导航目标，会按需重新测量该行，并把驻留项登记到同一回收集。

Hydrate 只重建已有的行段，不重新决定换行边界，也不改行宽、top、height、runs 或装饰。样式行会以该段已有的 holes、runs、字体和字号重新构造 caret 停靠点；缩进和行首 glyph 的水平偏移按原测量路径补回。表格行不参与 caret 数组延迟，因为一个源行的多个单元格续段可能共享字节范围却有不连续的 caret 子集。

### 宽度与换行

`core::TextPrimitive::measureTextWidth` 使用 width-only 度量入口。命中完整度量缓存时直接取已缓存宽度；冷路径塑形只累计字形 advance 并返回宽度，不会把缺少 caret 数组的半成品写进完整 `TextMetrics` 缓存。

无样式、未溢出当前 viewport 的行可走 width-only 路径；样式行可按各自字体和字重累计各段宽度。若行宽超过软换行宽度，测量会退回完整字形 metrics / `layoutStyledRange`，以原 caret 停靠点决定换行段，再释放不需要常驻的普通行 caret 数组。因而长的单个物理行在确定换行边界时仍可能产生完整的临时度量；表格测量也保留完整 caret 数据。宽度结果遵从既有塑形和 advance 累加口径；本机制不近似替换现有换行算法。

## 行装饰的不可变分页快照

`LineDecorationTable` 在至少 4096 行时按 64 行一页检查 uniform page。页内所有行必须在字段相等之外，对几何浮点字段作严格 `==` 比较；语义比较中允许的亚像素容差不能用于共享存储，否则会丢失微小但真实的几何差异。当前严格检查覆盖字号、行高、缩进、文字下移、块间距、图片尺寸、单元格 padding、glyph advance 和盒样式的 radius/bar width。

当 uniform 页数至少达到全部页数的四分之一（含恰好四分之一），快照会采用分页存储：uniform 页只持有一份精确行值，非 uniform 页保留逐行数据。达不到该门槛时保留连续 base vector，避免为稀少压缩收益重打包整张表。相等性包含字段内容，不能只按“普通段落”或行号推测 uniform。

快照以 `shared_ptr<const LineDecorationTable>` 发布。`replacing` 要求有序、唯一、有效的行号，并创建新代；它只重建被触及的页，页内复制未修改行，未触及页保持共享。之前发布的 generation 不变。`residentCapacityBytes()` 以页目录、页容器及嵌套动态容量计算分页快照驻留估算。

## 标签页派生缓存与预算

切页时，LP plan、装饰快照、源码高亮缓存和输入布局状态随其文档页搬迁，避免将 A 页的缓存配给 B 页。最近使用且预算内的非活动页可暖切复用；缓存缺失时可从正文及当前设置重建。预算只约束可丢弃的派生缓存，不会删除文档正文、撤销/重做历史或光标等编辑状态。

默认限制为非活动页派生缓存合计 64 MiB、最多两个持有派生缓存的非活动页。环境变量 `NEO_TAB_CACHE_BUDGET_MIB` 和 `NEO_TAB_CACHE_PAGES` 可在本地实验中覆盖这两个限制。超限时按最近使用时间逐出最旧页，逐出时释放布局行与 caret 数组、表格列数组和索引、详细行跟踪、几何前缀存储及缓存正文副本，并使布局缓存失效；下一次激活按需重建。编辑状态和 undo/redo 不随逐出清除。

记账使用容器 capacity 而不只用 size，并深入计入 plan 的当前/上一代、源码行与 token、装饰快照驻留容量、输入行和嵌套 runs/holes、caret 数组、表格列、几何数组、字符串容量及索引节点估算。InputState 与 LP DecorationCache 共用同一装饰 generation 时避免重复计入该 generation；legacy rows 则单独计入。标准库哈希节点和分配器元数据只能近似估算，因此该预算是对应用对象存储的工程估计，不是进程 RSS、提交量或系统分配器保留量的硬上限。释放对象容量也不保证操作系统立即降低进程 working set。

## 冷路径与性能边界

压缩减少长文档常驻的逐字符 caret 数组，并减少冷测量中为普通不换行行构造 caret 表的工作；行表、几何前缀、源码计划和表格结构仍需要完整文档级成本。首次布局仍会同步遍历全文。字体或 viewport 宽度变化、布局模式跨阈值、表格列变化、不能证明安全的装饰变化，以及冷缓存重建，仍可能触发全文布局或表格计算。正文编辑按已有增量契约尝试局部更新，判据不成立时回退完整路径。任何 worker/异步布局都不属于本说明描述的实现。

因此，切页复用与窗口滚动的预期收益需要分别测量；不能从 caret 数组容量下降推导出 CPU 延迟、RSS 或提交量按同一比例下降。特别是复杂表格、超长单行、resize 和首次打开要作为单独场景观察。

## 实验开关

- `NEO_VIEWPORT_METRICS_OFF`：在 NeoEditor 编辑器调用点关闭 viewport caret metrics，便于同一文稿对照。
- `NEO_COMPACT_DECORATIONS_OFF`：关闭 4096 行以上装饰表的 uniform-page 打包。
- `NEO_TAB_CACHE_BUDGET_MIB`、`NEO_TAB_CACHE_PAGES`：覆盖非活动页缓存预算和页数上限。

这些是实验/诊断入口，不是面向用户的设置或稳定兼容接口。试验前应记录 EXE 哈希、文档、字体、窗口尺寸、缩放、主题、开关和进程退出状态；比较时一次只切换一个变量。不要通过强制 trim working set 来制造释放效果。

## 回归测试入口

以下是源码中的测试入口，不在此处宣称本说明编写时的测试状态：

- `tests/unit/input_viewport_metrics.cpp`：比较 full/compact 的行数、宽度、top、行高、总高度、selection rect、随机 x/hit-test、左右/上下/Page 导航、滚动后的数组驻留上界，以及 IME、编辑、DPI 场景。
- `tests/unit/decoration_uniform_pages.cpp`：uniform 压缩、四分之一页门槛、稀疏 holes/runs/cells/image/fold 元数据、微小浮点几何差异保真、尾页与 immutable replacement。
- `tests/unit/document_tab_cache.cpp`：跨页缓存归属、warm plan 复用、页数和字节预算逐出、几何/表格容量回收，并确认 dirty 正文与 undo history 保留。
- `tests/unit/lp_decorations.cpp` 与 `tests/unit/text_metrics_cache.cpp`：装饰构建/缓存及文本宽度、完整度量缓存行为。

其他编辑器可迁移的核心判据是：只有在完整几何已定型后才省略可重建的字符级数据；每个按需查询入口必须在读取数组前 hydrate；异构或结构特殊的行类型可以保留完整路径；缓存共享必须依赖精确内容/几何证明；淘汰只能清派生数据并留住权威编辑状态；性能结论由真实场景基准支持，而不是由代码路径推断。
