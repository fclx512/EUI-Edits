# EUI-Edits @NEO_EDITOR_VERSION@ — 发布说明 / Release notes

**状态：0.1.0 已发布。** 正式包为静态 CRT 单 EXE，见 GitHub Release 附件；SHA256 见旁置 `.sha256` 文件。下文说明继续适用于该版本。

**Status: 0.1.0 is released.** The official package is a static-CRT single EXE; see the GitHub Release assets, with the SHA256 in the adjacent `.sha256` file. The notes below apply to that version.

## 产品内容 / Product

EUI-Edits 面向 Windows，聚焦轻量原生桌面体验。它以低占用为设计目标，采用现代 Windows UI；Markdown 将实时渲染与原位编辑结合，支持常见文档写作，同时提供基础文本编辑、查找替换、文件打开和文档库。它不是知识库产品，也不承诺完整 Obsidian 兼容。

EUI-Edits is a Windows editor focused on a lightweight native desktop experience. Designed for low resource use, it has a modern Windows UI. Markdown combines live rendering and in-place editing for common writing, alongside basic text editing, find and replace, file opening, and a document library. It is not a knowledge-base product and does not promise full Obsidian compatibility.

- 图片预览支持滚轮缩放和中键拖动平移；文档库可用 F2 重命名，并在名称冲突时生成编号名称。
- 查找栏打开时，Ctrl+G 跳到下一处，Ctrl+Shift+G 跳到上一处。
- 文件关联由应用设置选择，Windows 默认应用仍由 Windows 管理。用户设置与恢复副本保存在 `%APPDATA%/EUI-Edits`。
- Image preview supports wheel zoom and middle-button drag to pan. Press F2 in the document library to rename; conflicting names receive a numbered filename.
- With the find bar open, Ctrl+G goes to the next match and Ctrl+Shift+G to the previous one.
- Choose file associations in app settings; Windows continues to manage the default app separately. User settings and recovery copies are stored in `%APPDATA%/EUI-Edits`.

## 分发与许可 / Distribution and licenses

候选分发形式为单个 Windows x64 EXE，运行不需要外置 assets 目录。可旁置可选 `.exe.sha256` 文件核对可执行文件。许可证文本嵌入应用，可在“设置 → 关于”查看和导出；源码与文档保留在仓库。具体候选文件仍由主代理验收，本文不记录尚未完成的最终验证结果。

The candidate distribution is a single Windows x64 EXE and does not require an external assets directory. An optional adjacent `.exe.sha256` file can verify the executable. License texts are embedded and can be viewed or exported from Settings → About; source and documentation remain in the repository. The candidate files are still being checked by the lead agent, so these notes do not state unfinished final verification results.

## 范围与限制 / Scope and limits

默认打开上限为 64 MiB；疑似二进制或无法可靠解码的文件会被拒绝。文档库首次完整扫描同步进行，大型目录可能需要等待。Markdown 覆盖常见语法；公式、远程图片和演示模式不在当前范围。实机与 GUI 验收尚在进行，结果将以之后的验收报告为准。

The default file-open limit is 64 MiB; suspected binary files and files that cannot be decoded reliably are rejected. The initial full document-library scan is synchronous and may take time for large directories. Markdown covers common syntax; formulas, remote images, and presentation mode are outside the current scope. Machine and GUI acceptance is still in progress; a later acceptance report will record its results.
