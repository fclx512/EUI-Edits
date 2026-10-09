# EUI-Edits 项目协作约定

## 起点与范围

- 先执行 `git status --short`，阅读与当前任务相关的 `docs/` 规划和验收记录，再核对源码。旧文档中的版本、门禁数量、候选路径和状态可能过时，以当前源码及绑定时间的证据为准。
- 保留已有未提交文件、未跟踪文件和 `out/` 证据。不要 reset、clean、覆盖既有输出目录或宽泛暂存。提交、推送、打标签和发布须有用户明确授权；普通调查、实现、构建和验证在任务范围内直接进行。
- 对外名称为 EUI-Edits。保留活动内部标识 `neo_editor`、`NEO_EDITOR_VERSION`、`neoeditor-v*`；产品版本在 `cmake/NeoEditorVersion.cmake`，不要因产品改名修改 EUI-NEO 框架版本。

## 代码与性能

- 编辑器入口为 `apps/neo_editor/app.cpp`；文档视图在 `apps/neo_editor/ui/editor_view.h`，输入组件与模型在 `components/input.h`、`components/input_model.h`。布局、装饰、源字节/可见字节/视觉行的映射须保持一致。
- 性能工作先测当前基线，再确定主要成本，一次处理一个有证据的目标。使用同规模、同配置、同轨迹 A/B；记录原始轮次、预热、指标口径、范围、中位数及源码/EXE SHA256。
- 不以微基准推断整体 CPU、真实鼠标到屏幕延迟或低配性能。不以 `EndDraw` 成功证明帧已经显示。嵌套阶段时间不可直接相加；工作集、私有工作集、私有提交不可相加。
- 缓存必须覆盖文档身份、文本、装饰、字体、宽度及相关版本。不得以不完整 revision 键跳过外部回写、换页、撤销、IME 或布局变化。保留 UTF-8、隐藏标记、中文/emoji、反向和跨视口选区正确性。

## Luna 分工

- 用户要求 Luna 协助时，使用 `gpt-6-luna` 做有界只读源码/证据盘点、指定测试和独立文档杂务。明确允许修改的文件、禁止范围和交付物。
- 默认不让 Luna 修改生产代码、整合现有 dirty 工作树或执行发布。主代理独立核对源码、测量方法、变更清单及结论；Luna 的线索不等于已测热点。
- 多代理共用文件系统，安排互不重叠的文件；未知来源的改动须核查，不能删除。

## 构建与检查

- Windows / PowerShell；本仓库使用 Win32 / Direct2D / MSVC x64 Release。构建目录须属于当前仓库；不要复用其他 checkout 的 CMake cache。创建新证据目录，保留已有结果。
- Python：`D:/Python312/python.exe`。翻译检查为 `tests/tools/check_i18n.py`。
- CMake 不在 PATH 时可用 `D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`，使用前检查存在；脚本也支持通过 vswhere 定位。
- 有针对性地构建并运行相关测试。单元测试源文件由 CMake 自动发现；新增关键回归需核对是否纳入 `scripts/check-neoeditor.ps1` 的正式集合。
- 完整正确性门禁示例（目录必须不存在）：

```powershell
./scripts/check-neoeditor.ps1 -BuildDirectory ./out/check-unique/build `
  -CMake 'D:/ruanjian/Microsoft Visual Studio/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' `
  -Python D:/Python312/python.exe -Parallel 6
git -c core.whitespace=cr-at-eol diff --check
```

- 正确性、性能、真实窗口和独立打包分开验收。正式包使用 `scripts/package-neoeditor.ps1`，明确版本和全新构建/输出目录，验证最终资产 SHA256；开发 EXE 不改名冒充发布包。

## 证据与文档

- `out/` 保存本轮构建、日志、原始数据和哈希；`docs/` 保存可复查的方法、结果、剩余事项与命令。不要修改历史报告来冒充新一轮通过。
- GUI 操作按当前可用 computer-use 技能执行；绑定本轮目标 EXE、独立配置、所属窗口和正常退出。工具不能执行的轨迹标为 SKIP；无头模型测试不替代真实窗口验收。
- 区分 PASS、FAIL、OBSERVED、SKIP、NOT_RUN；注明 GPU、低配、IME、多屏 DPI 及持续拖选的实际覆盖。历史 OPT-001/OPT-002 未复现并不等于已解决。
- 文档核对先中文后英文，描述实际行为和条件，避免无证据的普遍收益或固定内存上限。
