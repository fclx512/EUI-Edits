# EUI-Edits @NEO_EDITOR_VERSION@ — 发布说明 / Release notes

**EUI-Edits 0.1.1 版本说明。**

**EUI-Edits 0.1.1 release notes.**

## 本次更新 / Changes

- **表格与图片编辑：** 修正表格单元格中的光标映射；支持粘贴位图、PNG 和受支持的图片文件，并可从文件选择器导入图片。插入后立即预览，支持单步撤销。
- **长行输入：** 只为横向视口附近的着色片段创建文字节点，同时保留完整正文、选区和命中度量；输入回调共享只读布局，减少长行度量的重复复制；拖选位置和滚动状态不变时，不再重复请求构建。
- **文档库：** 启动或打开文档时优先复用目录快照并后台扫描，正文可先于目录扫描结果呈现；快照字节估算移到后台扫描工作中，减少主线程发布快照时的工作。
- **Win32 消息处理：** 消息队列持续积压时分批处理，让主循环有机会继续更新和绘制。

长行与目录数据来自指定开发机和测试条件：横向视口过滤降低了两个单行 JSON 样本的闲置 CPU 与专用提交量；回调共享布局降低了测试中复制七个回调时的分配量；D50 样本从进程创建到首篇正文构建结束的三轮中位数由 2.162 秒降至 0.107 秒，这不代表窗口首次可见或可输入时间。拖选总体 CPU 区间仍重叠，静止拖选测试只确认重复构建请求减少；不同文档和设备的收益会有差异。测量条件与报告见[发版交接](../../docs/EUI-Edits-性能优化进度与发版交接-2026-10-08.md)及其中链接的各批结果。

Win32 消息泵测试确认 5000 条自有普通消息被分 20 批完整有序处理；2 ms 是回调之间检查的软预算，不是硬响应上限。原始 OPT-001 高频滚轮停滞仍未复现并定位根因，本次不宣称已修复。低配设备、硬件 GPU、真实 IME、多显示器 DPI 和长时间压力仍有未覆盖项。

## Changes

- **Tables and images:** Corrected cursor mapping within table cells. Pasting bitmap, PNG, and supported image files is supported, as is importing an image through the file picker. Inserted images are previewed immediately and can be undone in one step.
- **Long-line input:** Text nodes are created only for colored fragments near the horizontal viewport, while the complete document, selection, and hit-test metrics are retained. Input callbacks share a read-only layout to reduce repeated copying of long-line metrics. Dragging no longer requests another build when the drag position and scroll state have not changed.
- **Document library:** On startup or when opening a document, the app reuses a directory snapshot when available and scans in the background so the document can be presented before scanning completes. Snapshot byte estimation is performed by the background scan work to reduce main-thread work when publishing a snapshot.
- **Win32 message handling:** When the message queue remains busy, messages are processed in batches so the main loop can continue updating and drawing.

Long-line and library results use specified development-machine test conditions. Horizontal viewport filtering reduced idle CPU and private commit for two single-line JSON samples; shared callbacks reduced allocations when copying seven callbacks in the test. For the D50 sample, the median across three rounds from process creation to the first document build changed from 2.162 seconds to 0.107 seconds; this does not measure first window visibility or input readiness. Overall drag-selection CPU ranges still overlap, and the stationary-drag test confirms fewer repeated build requests only. Results vary by document and device. See the [release handoff](../../docs/EUI-Edits-性能优化进度与发版交接-2026-10-08.md) and its linked batch reports for methods and evidence.

The Win32 message-pump test processed 5,000 ordinary messages posted to a test-owned window in 20 complete, ordered batches. The 2 ms budget is checked between callbacks and is not a hard response-time limit. The original OPT-001 high-frequency wheel stall has not been reproduced and its cause remains unconfirmed; this release does not claim to fix it. Low-end devices, hardware GPU, real IME, multi-monitor DPI, and prolonged stress remain uncovered.

## 产品 / Product

EUI-Edits 面向 Windows，聚焦轻量原生桌面体验。Markdown 将实时渲染与原位编辑结合，支持常见文档写作，同时提供基础文本编辑、查找替换、文件打开和文档库。它不是知识库产品，也不承诺完整 Obsidian 兼容。

EUI-Edits is a Windows editor focused on a lightweight native desktop experience. Markdown combines live rendering and in-place editing for common writing, alongside basic text editing, find and replace, file opening, and a document library. It is not a knowledge-base product and does not promise full Obsidian compatibility.

- 图片预览支持滚轮缩放和中键拖动平移；文档库可用 F2 重命名，名称冲突时提供编号名称建议，确认后再重命名。
- 查找栏打开时，Ctrl+G 跳到下一处，Ctrl+Shift+G 跳到上一处。
- 文件关联由应用设置选择，Windows 默认应用仍由 Windows 管理。用户设置与恢复副本保存在 `%APPDATA%/EUI-Edits`。
- Image preview supports wheel zoom and middle-button drag to pan. Press F2 in the document library to rename; on a name conflict, a numbered filename is suggested and renaming waits for confirmation.
- With the find bar open, Ctrl+G goes to the next match and Ctrl+Shift+G to the previous one.
- Choose file associations in app settings; Windows continues to manage the default app separately. User settings and recovery copies are stored in `%APPDATA%/EUI-Edits`.

## 分发与许可 / Distribution and licenses

Windows x64 发行包采用单个 EXE，框架和 MSVC 运行时静态链接，无需外置 assets 目录。许可证文本嵌入应用，可在“设置 → 关于”查看和导出；旁置 `.exe.sha256` 文件可用于校验可执行文件。

The Windows x64 distribution uses a single EXE with the framework and MSVC runtime statically linked; no external assets directory is required. License texts are embedded and can be viewed or exported from Settings → About. An adjacent `.exe.sha256` file can verify the executable.

## 范围与限制 / Scope and limits

默认打开上限为 64 MiB；疑似二进制或无法可靠解码的文件会被拒绝。文档库初次加载在后台扫描目录与文件元数据，不预先读取正文；部分显式文件操作仍同步刷新列表，大型目录可能需要等待。Markdown 覆盖常见语法；公式、远程图片和演示模式不在当前范围。功能边界与带测试条件的内存数据见[仓库 README](../../README.md)。

The default file-open limit is 64 MiB; suspected binary files and files that cannot be decoded reliably are rejected. Initial library loading scans directory and file metadata in the background without pre-loading document contents; some explicit file operations still refresh the list synchronously and may take time for large directories. Markdown covers common syntax; formulas, remote images, and presentation mode are outside the current scope. See the [repository README](../../README_EN.md) for feature limits and memory measurements with their test conditions.
