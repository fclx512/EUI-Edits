# EUI-Edits 项目协作约定

## 接手

- 先运行 `git status --short`，再读 [开发与维护规范](docs/开发流程与交接规范.md)、[文档导航](docs/目录说明.md)、[当前路线](docs/当前实现与验收路线.md)和任务相关契约，最后核对源码。
- 保留已有修改、未跟踪文件和 `out/` 证据。普通调查、实现、构建与验证直接进行；提交、推送、标签和发布须有用户明确授权。
- 对外名称为 EUI-Edits；内部保留 `neo_editor`、`NEO_EDITOR_VERSION`、`neoeditor-v*`。产品版本源是 `cmake/NeoEditorVersion.cmake`，与 EUI-NEO 框架版本分别维护。

## 实现与验证

- 编辑器入口为 `apps/neo_editor/app.cpp`，文档视图为 `apps/neo_editor/ui/editor_view.h`，输入组件与模型为 `components/input.h`、`components/input_model.h`。源字节、可见字节、视觉行与几何映射须一致。
- 缓存覆盖文档身份、正文、装饰、字体、宽度及相关版本；核对外部回写、切页、撤销、IME、中英/emoji、反向和跨视口选区。
- 性能任务先测当前基线，同规模、同配置、同轨迹 A/B，记录预热、原始轮次、指标口径、中位数与源码/EXE SHA256。组件计时与整窗响应分别报告；嵌套阶段时间和不同内存指标分别列出。
- Windows / PowerShell 7、MSVC x64 Release、Win32 / Direct2D。Python 为 `D:/Python312/python.exe`。构建目录属于当前仓库；本机 CMake 路径及命令见[构建参数](docs/NeoEditor-发布构建说明-2026-10-02.md)。
- 小改动用定向检查、增量构建和 `out/preview-1..3` 轮换预览；交付前运行 `scripts/check-neoeditor-preview.ps1`。完整检查与可用版本按开发规范选择，新关键回归核对 `scripts/check-neoeditor.ps1` 的正式集合。
- GUI 按当前 computer-use 技能执行，绑定目标 EXE、独立配置、所属窗口及正常退出。正确性、性能、真实窗口和独立包分别验收；未覆盖的设备、IME、DPI 或拖选轨迹简记 `SKIP` / `NOT_RUN`。
- 发版先读[发版流程](docs/发版流程.md)，统一使用 `scripts/prepare-neoeditor-release.ps1`，先预检后完整准备，默认 `-Parallel 2`。由一个主代理冻结和构建候选；推源后先等该 SHA 的 Windows checks，再按授权打标签。最终资产哈希变化时重新绑定 GUI。

## 文档与收尾

- 行为、契约或命令变化时，同轮更新对应文档；同步范围由 `docs/文档清单.json` 维护。先中文后英文，先写实际行为，仅在未实测或证据不足处简述边界。
- 长期规则归开发规范，发布顺序归发版流程，当前状态页只保留能力地图和后续事项。具体轮次、测量数据和历史结论留在 `local-docs/` 与 `out/`。
- 代码改动后用 `D:/Python312/python.exe scripts/check-docs.py --write-review` 生成待核对记录，逐项核对并填写 `docs/文档同步记录.json` 的更新内容或无需更新的原因。代码再次变化后重新生成并核对；新增源码目录补充同步映射。
- 收尾运行 `D:/Python312/python.exe scripts/check-docs.py` 和 `git -c core.whitespace=cr-at-eol diff --check`，再生成本地交接快照。默认比较 HEAD，未完成同步核对会失败；清单变化后先用 `--write-index` 更新导航。文档 CI 检查整个 push / PR 的源码范围。
- `out/` 的轮换、候选保护、归档保留和本轮构建树清理统一按开发规范第 4 节执行，保留已有证据。
